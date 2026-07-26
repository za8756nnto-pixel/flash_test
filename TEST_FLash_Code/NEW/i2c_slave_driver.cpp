//#############################################################################
// FILE:   i2c_slave_driver.cpp
//#############################################################################
#include "i2c_slave_driver.h"

I2cSlaveDriver &g_i2cSlave = I2cSlaveDriver::instance();

//=============================================================================
// コンストラクタ / 初期化
//=============================================================================
I2cSlaveDriver::I2cSlaveDriver()
    : base_(0)
    , state_(State::IDLE)
    , rxIndex_(0)
    , expectedLen_(0)
    , frameActive_(false)
    , elapsedMs_(0)
    , crcErrorCount_(0)
    , lenMismatchCount_(0)
    , timeoutCount_(0)
{
}

void I2cSlaveDriver::init(uint32_t base)
{
    base_ = base;

    // モジュールをリセット(設定変更はモジュール停止中に行う)
    I2C_disableModule(base_);

    // スレーブモード / 7bitアドレッシング
    I2C_initSlaveMode(base_, I2C_SLAVE_ADDRESS, I2C_ADDR_MODE_7BITS);

    // 受信専用として使うが、マスタからの読み出し要求に備えて最低限
    // ターゲット送信モードも許容できるよう基本設定のみ実施
    I2C_setConfig(base_, I2C_SLAVE_RECEIVE_MODE);
    I2C_setBitCount(base_, I2C_BITCOUNT_8);

    // FIFOは使わず1バイトずつ RRDY 割込みで処理する(基本割込みのみ使用)
    I2C_disableFIFO(base_);

    // 割込み要因:
    //   ADDR_SLAVE      : 自アドレスに一致(START/RESTART)
    //   RX_DATA_RDY      : 1バイト受信完了
    //   STOP_CONDITION   : STOP検出
    //   NO_ACK           : NACK検出(異常)
    //   ARB_LOST         : 調停負け(マルチマスタ構成でなくても保険で監視)
    I2C_enableInterrupt(base_,
                         I2C_INT_ADDR_SLAVE |
                         I2C_INT_RX_DATA_RDY |
                         I2C_INT_STOP_CONDITION |
                         I2C_INT_NO_ACK |
                         I2C_INT_ARB_LOST);

    I2C_enableModule(base_);

    resetState();
}

void I2cSlaveDriver::resetState()
{
    state_        = State::IDLE;
    rxIndex_      = 0;
    expectedLen_  = 0;
    frameActive_  = false;
    elapsedMs_    = 0;

    current_.cmd     = 0;
    current_.dataLen = 0;
    current_.format  = I2cFrameFormat::FORMAT_LEN;
    current_.crcOk   = false;
}

//=============================================================================
// I2C割込みハンドラ本体
// PIEベクタから呼ばれる isr() のラッパ関数(i2cSlaveBasicISR)から呼び出す
//=============================================================================
void I2cSlaveDriver::isr()
{
    I2C_InterruptSource intSource = I2C_getInterruptSource(base_);

    switch (intSource)
    {
        case I2C_INTSRC_ADDR_SLAVE:
            handleAddrSlave();
            break;

        case I2C_INTSRC_RX_DATA_RDY:
            handleRxDataReady();
            break;

        case I2C_INTSRC_STOP_CONDITION:
            handleStopCondition();
            break;

        case I2C_INTSRC_NO_ACK:
            handleNack();
            break;

        case I2C_INTSRC_ARB_LOST:
            handleArbLost();
            break;

        case I2C_INTSRC_REG_ACCESS_RDY:
        case I2C_INTSRC_TX_DATA_RDY:
        case I2C_INTSRC_NONE:
        default:
            // 本ドライバでは未使用の要因。読み捨てておく。
            break;
    }
}

//-----------------------------------------------------------------------------
// AAS: アドレス一致(新規START、またはRESTART後の再アドレッシング)
//-----------------------------------------------------------------------------
void I2cSlaveDriver::handleAddrSlave()
{
    // マスタからの書き込み(スレーブ受信)以外は本ドライバの対象外
    if (I2C_getStatus(base_) & I2C_STS_SLAVE_DIR)
    {
        // マスタが読み出し要求(スレーブ送信)を出してきた場合はここでは扱わない。
        // 必要であれば I2C_setConfig(base_, I2C_SLAVE_TRANSMIT_MODE) 等で
        // 別途応答処理を実装すること。
        return;
    }

    switch (state_)
    {
        case State::IDLE:
            // 新しいフレームの開始
            resetState();
            state_       = State::WAIT_CMD;
            frameActive_ = true;
            elapsedMs_   = 0;
            break;

        case State::WAIT_LEN_OR_RESTART:
            // CMD受信直後にAASが再度来た = RESTARTが発生した
            // => Format B (RESTART後、可変長のDATAを受信)へ遷移
            current_.format = I2cFrameFormat::FORMAT_RESTART;
            state_           = State::WAIT_DATA_B;
            rxIndex_         = 0;
            elapsedMs_       = 0; // RESTART検出でタイムアウトカウンタを再スタート
            break;

        default:
            // それ以外の状態でのAASはプロトコル違反(想定外の再START等)。
            // 受信途中のデータは破棄し、これを新しいフレームの開始として
            // 仕切り直す。
            resetState();
            state_       = State::WAIT_CMD;
            frameActive_ = true;
            elapsedMs_   = 0;
            break;
    }
}

//-----------------------------------------------------------------------------
// RRDY: 1バイト受信完了
//-----------------------------------------------------------------------------
void I2cSlaveDriver::handleRxDataReady()
{
    uint16_t byteVal = I2C_getData(base_);
    uint8_t  b        = static_cast<uint8_t>(byteVal);

    // バイトを受信できた時点でタイムアウトカウンタをリセット
    elapsedMs_ = 0;

    switch (state_)
    {
        case State::WAIT_CMD:
            current_.cmd = b;
            // 次に来るのがLEN(Format A)かRESTART(Format B)かはまだ不明。
            // RRDYが先に来ればFormat AのLEN、AASが先に来ればFormat B。
            state_ = State::WAIT_LEN_OR_RESTART;
            break;

        case State::WAIT_LEN_OR_RESTART:
            // RESTARTが挟まらずに次のデータが来た => Format A の LEN バイト
            current_.format = I2cFrameFormat::FORMAT_LEN;
            expectedLen_     = b;              // LEN(ペイロード+CRC8の合計バイト数として扱う)
            rxIndex_         = 0;

            if (expectedLen_ == 0U)
            {
                // LEN=0は不正値として即破棄(タイムアウトを待たず破棄)
                lenMismatchCount_++;
                resetState();
            }
            else if (expectedLen_ > I2C_PKT_MAX_DATA)
            {
                // バッファ上限を超えるLENも不正値として破棄
                lenMismatchCount_++;
                resetState();
            }
            else
            {
                state_ = State::WAIT_DATA_A;
            }
            break;

        case State::WAIT_DATA_A:
            if (rxIndex_ < I2C_PKT_MAX_DATA)
            {
                current_.data[rxIndex_] = b;
                rxIndex_++;
            }
            // LENで指定された数を受信し終えたらSTOP待ちへ。
            // 万一マスタがLENを超えて送ってきた場合は、超過分は捨てつつ
            // カウンタだけ進め、STOP時にLEN不一致として破棄する。
            if (rxIndex_ >= expectedLen_)
            {
                state_ = State::WAIT_STOP_ONLY;
            }
            break;

        case State::WAIT_DATA_B:
            // Format B: 長さはSTOPが来るまで不定。バッファ上限までは格納する。
            if (rxIndex_ < I2C_PKT_MAX_DATA)
            {
                current_.data[rxIndex_] = b;
                rxIndex_++;
            }
            else
            {
                // 上限超過。以降は破棄対象としてマークしておく。
                lenMismatchCount_++;
            }
            break;

        case State::WAIT_STOP_ONLY:
            // LEN分受信済みなのに追加でデータが来た(想定外)。
            // 不一致としてカウントするのみ。STOPで破棄される。
            break;

        case State::IDLE:
        default:
            // フレーム開始前にRRDYが来ることは通常ないが、保険として無視。
            break;
    }
}

//-----------------------------------------------------------------------------
// SCD: STOP検出。ここでフレームを確定させ、問題なければリングバッファへpush
//-----------------------------------------------------------------------------
void I2cSlaveDriver::handleStopCondition()
{
    bool ok = true;

    switch (state_)
    {
        case State::WAIT_STOP_ONLY:
            // Format A: 受信数とLENが一致しているか確認
            if (rxIndex_ != expectedLen_)
            {
                lenMismatchCount_++;
                ok = false;
            }
            break;

        case State::WAIT_DATA_B:
            // Format B: 最低でもCRCバイト1つは必要
            if (rxIndex_ < 1U)
            {
                lenMismatchCount_++;
                ok = false;
            }
            break;

        default:
            // WAIT_CMD, WAIT_LEN_OR_RESTART, WAIT_DATA_A(規定数未満) 等で
            // STOPが来た場合は、途中終了(不完全フレーム)として破棄。
            ok = false;
            break;
    }

    finalizeAndPush(ok);
    resetState();
}

//-----------------------------------------------------------------------------
// NACK / 調停負け: いずれも異常系。受信中データは破棄してリセット。
//-----------------------------------------------------------------------------
void I2cSlaveDriver::handleNack()
{
    resetState();
}

void I2cSlaveDriver::handleArbLost()
{
    resetState();
}

//-----------------------------------------------------------------------------
// フレーム確定処理: CRC検証を行った上でリングバッファへpushする
//-----------------------------------------------------------------------------
void I2cSlaveDriver::finalizeAndPush(bool completedNormally)
{
    if (!completedNormally || rxIndex_ == 0U)
    {
        // LEN不一致 or 不完全フレーム: このデータは破棄する(pushしない)
        return;
    }

    // 受信データの最後の1バイトをCRC8として扱う
    uint16_t payloadLen = static_cast<uint16_t>(rxIndex_ - 1U);
    uint8_t  rxCrc       = current_.data[payloadLen];

    // CRC計算対象: CMD (+ Format Aの場合はLENも含める) + ペイロード
    uint8_t crcBuf[2U + I2C_PKT_MAX_DATA];
    uint16_t crcLen = 0;

    crcBuf[crcLen++] = current_.cmd;
    if (current_.format == I2cFrameFormat::FORMAT_LEN)
    {
        crcBuf[crcLen++] = static_cast<uint8_t>(expectedLen_);
    }
    for (uint16_t i = 0; i < payloadLen; i++)
    {
        crcBuf[crcLen++] = current_.data[i];
    }

    uint8_t calcCrc = crc8(crcBuf, crcLen);
    bool crcOk = (calcCrc == rxCrc);

    if (!crcOk)
    {
        crcErrorCount_++;
    }

    // ペイロード(CRCバイトを除いた実データ)としてまとめてpush
    current_.dataLen = payloadLen;
    current_.crcOk   = crcOk;

    // NOTE: CRC不一致でも上位層で判断できるよう、ここではpushする。
    //       「CRC不一致は完全に破棄したい」場合は下記をコメントアウトし、
    //       if (crcOk) の中でのみ ring_.push() するよう変更する。
    ring_.push(current_);
}

//-----------------------------------------------------------------------------
// CRC-8 (多項式 0x07, 初期値 0x00) ソフトウェア実装
//-----------------------------------------------------------------------------
uint8_t I2cSlaveDriver::crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00U;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            if (crc & 0x80U)
            {
                crc = static_cast<uint8_t>((crc << 1) ^ 0x07U);
            }
            else
            {
                crc = static_cast<uint8_t>(crc << 1);
            }
        }
    }
    return crc;
}

//=============================================================================
// タイムアウト監視 (1msティックから呼び出す)
//=============================================================================
void I2cSlaveDriver::tick1ms()
{
    if (!frameActive_)
    {
        return;
    }

    if (state_ == State::IDLE)
    {
        frameActive_ = false;
        return;
    }

    elapsedMs_++;

    if (elapsedMs_ >= I2C_TIMEOUT_MS)
    {
        // STOPが来ないままタイムアウト -> 受信中データを破棄してリセット
        timeoutCount_++;

        // 割込みと1msティックが競合しないよう、状態変更中は割込み禁止にする
        DINT;
        resetState();

        // I2Cバスが本当にハングしている場合に備え、必要であればここで
        // I2Cモジュール自体を再初期化する(コメントアウトで用意)。
        // I2C_disableModule(base_);
        // I2C_enableModule(base_);
        EINT;
    }
}

//=============================================================================
// 割込みハンドラ (C形式ラッパ)
//=============================================================================
extern "C" __interrupt void i2cSlaveBasicISR(void)
{
    g_i2cSlave.isr();

    // PIEグループへACK(I2Cの基本割込みはPIEグループ8)
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
}

extern "C" void i2cSlaveTimeoutTick(void)
{
    g_i2cSlave.tick1ms();
}
