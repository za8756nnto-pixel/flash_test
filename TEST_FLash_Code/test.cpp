#include "Ci2cRoleSwitcher.hpp"
// #include "../uart/CuartApp.hpp"

extern CUartApp *Pc;

//=====================================================================
// 内部リングバッファ操作(生バイト用) ※変更なし
//=====================================================================
static inline void ring_init(Ci2cRoleSwitcher::Ring* r)
{
    r->head = r->tail = 0;
}

static inline bool ring_push(Ci2cRoleSwitcher::Ring* r, uint8_t v)
{
    uint16_t n = (r->head + 1) & 0xFF;
    if (n == r->tail) return false; // バッファフル
    r->buf[r->head] = v;
    r->head = n;
    return true;
}

//=====================================================================
// 内部リングバッファ操作(フレーム位置情報用) ※新設
//   ・実データはコピーせず、rxq_上の [start, length] だけを保持する
//   ・境界は STOP コンディション到来時にのみ確定させる
//     -> データ長フィールドがノイズで化けても、境界情報自体は無事
//=====================================================================
static inline void frame_init(Ci2cRoleSwitcher::FrameRing *q)
{
    q->head = q->tail = 0;
}

static inline bool frame_push(Ci2cRoleSwitcher::FrameRing *q, uint16_t start, uint16_t length)
{
    uint16_t n = (q->head + 1) & 0x0F;
    if (n == q->tail) return false; // フレームキューフル(取りこぼし)
    q->frame[q->head].start  = start;
    q->frame[q->head].length = length;
    q->head = n;
    return true;
}

static inline bool frame_pop(Ci2cRoleSwitcher::FrameRing *q, Ci2cRoleSwitcher::RxFrameInfo &f)
{
    if (q->head == q->tail) return false;
    f = q->frame[q->tail];
    q->tail = (q->tail + 1) & 0x0F;
    return true;
}

//=====================================================================
// コンストラクタ
//   ※ハードウェア初期化部は既存実装のものをそのまま流用してください。
//     ここでは新設したメンバの初期化のみ明記します。
//=====================================================================
Ci2cRoleSwitcher::Ci2cRoleSwitcher(uint32_t base)
    : base_(base)
    , role_(Target)
    , arbLost_(false)
    , stopIssued_(false)
    , stopDetected_(false)
    , nack_(false)
    , txDone_(false)
    , rx_stop_(false)
    , rx_tail_(0)
    , rxByteCount_(0)
    , frameStart_(0)
    , readPending_(false)
    , readPendingCmd_(0)
    , readTransaction_(false)
    , rxCmdType_(RX_CMD_NONE)
    , droppedFrameCount_(0)
{
    ring_init(&rxq_);
    frame_init(&frameq_);
    // txAddr / txParamAddr / txVersionAddr / txModeAddr の初期化、
    // レジスタ初期化などは既存コンストラクタ実装をそのまま維持してください。
}

//=====================================================================
// CRC8計算(仮実装)
//   実プロトコルで使用しているCRC多項式・初期値に置き換えてください。
//   ここでは一般的なCRC-8(多項式0x07, 初期値0x00)を例として置いています。
//=====================================================================
uint8_t Ci2cRoleSwitcher::calcCRC8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
        {
            if (crc & 0x80)
                crc = (uint8_t)((crc << 1) ^ 0x07);
            else
                crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

void Ci2cRoleSwitcher::switchToControllerReceive(uint16_t targetAddr, uint16_t len, bool repeat)
{
    waitBusIdle();
    resetFIFO();
    I2C_clearStatus(base_, 0xFFFF);

    I2C_setTargetAddress(base_, targetAddr);
    uint16_t cfg = I2C_CONTROLLER_RECEIVE_MODE;
    if (repeat) cfg |= I2C_REPEAT_MODE;
    I2C_setConfig(base_, cfg);

    if (!repeat) I2C_setDataCount(base_, len);

    role_ = Controller;
    clearFlags();
    I2C_enableFIFO(base_);
    I2C_sendStartCondition(base_);
}

//=====================================================================
// FIFOから取り出して受信用リングバッファ(rxq_)に積む
//   ★修正点: 従来は最初の1バイトのコマンド種別判定のみでバイト自体を
//     rxq_ に保存していなかった。フレーム全体を後で読み出せるように
//     受信した全バイトを rxq_ に push するよう修正。
//=====================================================================
void Ci2cRoleSwitcher::getFIFOData()
{
    while (I2C_getRxFIFOStatus(base_) != I2C_FIFO_RXEMPTY)
    {
        uint8_t b = (uint8_t)I2C_getData(base_);

        // ---- 生バイトをリングバッファへ格納 ----
        if (!ring_push(&rxq_, b))
        {
            // rxq_ が溢れた場合、以降このフレームは信用できないため破棄対象。
            // (STOP到来時、長さ整合チェック or CRCチェックで弾かれる)
        }
        rxByteCount_++;

        // コマンド先頭バイト判定(READ要求の振り分け用途はそのまま維持)
        if (rxByteCount_ == 1)
        {
            rxCmdType_ = RX_CMD_UNKNOWN;
            if ((b == 0x11) || (b == 0x13) || (b == 0x14))
            {
                readPending_    = true;
                readPendingCmd_ = b;

                switch (b)
                {
                case 0x11:
                    txAddr = txParamAddr;
                    break;
                case 0x13:
                    txAddr = txVersionAddr;
                    break;
                case 0x14:
                    txAddr = txModeAddr;
                    break;
                }
            }
        }
    }
}

void Ci2cRoleSwitcher::onBasicISR()
{
    I2C_InterruptSource intSource = I2C_getInterruptSource(base_);

    if (role_ == Target)
    {
        switch (intSource)
        {
        case I2C_INTSRC_ADDR_TARGET:
        {
            if ((I2C_getStatus(base_) & I2C_STS_TARGET_DIR))
            {
                if (readPending_ == true)
                {
                    readTransaction_ = true;
                }
                I2C_setConfig(base_, I2C_TARGET_SEND_MODE);
                (void)I2C_getStatus(base_); // ステータス切替のための空読み。削除しないこと。
                setDirectSend();
            }
            else
            {
                // ---- 受信トランザクション開始 ----
                rxCmdType_   = RX_CMD_NONE;
                rxByteCount_ = 0;
                txAddr.counter = 0;

                // ★このトランザクションの先頭位置を rxq_.head で記録
                //   -> STOP時にこの位置と受信バイト数からフレーム境界を確定する
                frameStart_ = rxq_.head;

                I2C_setConfig(base_, I2C_TARGET_RECEIVE_MODE);
            }
            break;
        }

        case I2C_INTSRC_TX_DATA_RDY:
        {
            setDirectSend();
            break;
        }

        case I2C_INTSRC_RX_DATA_RDY:
        {
            getFIFOData();
            break;
        }

        case I2C_INTSRC_STOP_CONDITION:  // /STOP
        {
            // RXFF が来なかった差分を必ず回収
            getFIFOData();

            I2C_setConfig(base_, I2C_TARGET_RECEIVE_MODE);
            I2C_clearInterruptStatus(base_,
                I2C_INT_ADDR_TARGET |
                I2C_INT_RX_DATA_RDY |
                I2C_INT_TX_DATA_RDY |
                I2C_INT_STOP_CONDITION
            );
            I2C_disableModule(base_);
            I2C_enableModule(base_);
            txAddr.counter = 0;
            stopDetected_  = true;

            if (readTransaction_)
            {
                readPending_     = false;
                readPendingCmd_  = 0;
                readTransaction_ = false;
            }
            else if (rxByteCount_ > 0)
            {
                // ★ここが今回の修正の核心。
                //   データ長フィールドの中身は一切信用せず、
                //   「STOPまでに実際に受信したバイト数(rxByteCount_)」を
                //   そのままフレーム長としてキューへ積む。
                //   これにより、データ長フィールドがノイズで化けていても
                //   フレーム境界自体は正しく確定される。
                if (!frame_push(&frameq_, frameStart_, rxByteCount_))
                {
                    // frameqが満杯(ポーリングが追いついていない)
                    // 最古のフレームを読み捨てて空きを作るか、
                    // ここでエラーカウンタをインクリメントする等の対応を検討。
                    droppedFrameCount_++;
                }
                rx_stop_ = true;
            }

            rxByteCount_ = 0;
            break;
        }

        default:
            break;
        }
    }
}

//=====================================================================
// フレーム単位でのポーリング取得(新設)
//   ・STOPで確定した1フレーム分を rxq_ から取り出す
//   ・以下の順でチェックし、いずれか失敗したら false を返して
//     そのフレームだけを読み捨てる(他のフレームには影響しない)
//     1) 最小/最大サイズ範囲内か
//     2) 長さフィールド(declLen) + ヘッダ + CRC == 実受信バイト数(f.length)
//     3) CRC一致
//=====================================================================
bool Ci2cRoleSwitcher::PopFrame(uint8_t *dst, uint16_t &len)
{
    RxFrameInfo f;
    if (!frame_pop(&frameq_, f))
        return false; // 取り出せるフレームなし

    // ---- サイズ範囲チェック ----
    if (f.length < FRAME_MIN_SIZE || f.length > FRAME_MAX_SIZE)
    {
        // rxq_ 側のtailだけ進めて読み捨てる
        rxq_.tail = (uint16_t)((f.start + f.length) & 0xFF);
        droppedFrameCount_++;
        return false;
    }

    // ---- rxq_ からローカルバッファへコピー ----
    uint8_t tmp[FRAME_MAX_SIZE];
    uint16_t pos = f.start;
    for (uint16_t i = 0; i < f.length; i++)
    {
        tmp[i] = rxq_.buf[pos];
        pos = (uint16_t)((pos + 1) & 0xFF);
    }
    rxq_.tail = pos; // rxq_を必ず解放(次のフレームのために進める)

    // ---- ヘッダ解釈 ----
    uint8_t cmd     = tmp[0];
    uint8_t declLen = tmp[1]; // フレーム内で申告されている「データ長」

    // ---- ★長さフィールドと実受信バイト数の整合性チェック ----
    //   期待される総フレームサイズ = ヘッダ(2) + データ長(declLen) + CRC(1)
    uint16_t expectedTotal = (uint16_t)declLen + FRAME_HEADER_SIZE + FRAME_CRC_SIZE;
    if (expectedTotal != f.length)
    {
        // 長さフィールドがノイズ等で化けている。
        // このフレームのみ破棄。他のフレーム・キューには一切影響しない。
        droppedFrameCount_++;
        return false;
    }

    // ---- CRCチェック ----
    uint8_t crcCalc = calcCRC8(tmp, f.length - FRAME_CRC_SIZE);
    uint8_t crcRecv = tmp[f.length - 1];
    if (crcCalc != crcRecv)
    {
        droppedFrameCount_++;
        return false;
    }

    (void)cmd; // 上位のパース処理でtmp[0]から再取得する想定。必要ならここで分岐してもよい。

    // ---- OK: 呼び出し元へコピーして返す ----
    for (uint16_t i = 0; i < f.length; i++)
    {
        dst[i] = tmp[i];
    }
    len = f.length;
    return true;
}

//=====================================================================
// ポーリング処理(新設・旧 I2cPoll() の置き換え)
//   ★旧I2cPoll()は「データ長フィールドを信じて1バイトずつ手動で
//     フレーム境界を作る」実装だったため、長さが化けると後続の
//     フレームまで巻き込んで崩壊していた。
//
//   新実装では境界確定を割り込み側(STOP)に完全に委譲し、
//   ポーリング側は「確定済みフレームを検証しながら取り出すだけ」
//   にすることで、1フレームの破損が他フレームに波及しないようにする。
//=====================================================================
static uint8_t s_frameBuf[FRAME_MAX_SIZE];

void I2cPoll(Ci2cRoleSwitcher &i2c)
{
    uint16_t frameLen = 0;

    // frameq_ に積まれている確定済みフレームを可能な限り処理する。
    // PopFrameがfalseを返しても、それは「今取り出したフレームが不正だった」
    // だけであり、キューに次のフレームが残っていればそのまま継続して良い。
    // (キューが空になったらループを抜けて次回のポーリングタイミングへ)
    while (true)
    {
        bool ok = i2c.PopFrame(s_frameBuf, frameLen);

        if (!ok)
        {
            // このタイミングでキューが本当に空なのか、
            // それとも「フレームはあったが検証NGで捨てた」のかは
            // PopFrameの戻り値だけでは区別できないため、
            // 継続してもう一度呼び出し、frameq_が完全に空になるまで回す。
            // (droppedFrameCount()の増減で監視すると判別可能)
            break; // ここでは簡略化のため1回のNGでいったん抜ける実装例
        }

        // ---- ここに到達した時点でフレームは検証済み(長さ一致・CRC OK) ----
        // s_frameBuf[0] = コマンド
        // s_frameBuf[1] = データ長
        // s_frameBuf[2 .. 2+len-1] = データ
        // s_frameBuf[frameLen-1] = CRC
        //
        // 例:
        // uint8_t cmd = s_frameBuf[0];
        // uint8_t declLen = s_frameBuf[1];
        // uint8_t *data = &s_frameBuf[2];
        // switch(cmd) { ... }
    }
}
