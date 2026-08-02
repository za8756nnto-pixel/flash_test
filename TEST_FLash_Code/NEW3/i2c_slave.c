/*
 * i2c_slave.c
 *
 * I2Cスレーブ受信・Flash書き込み制御
 *
 * 【変更点】
 * CMD/LENバイトのポーリング待ちを廃止し、ISR内にステートマシン
 * （WAIT_CMD -> WAIT_LEN -> RX_DATA）を実装。
 * メインループはI2Cレジスタを直接ポーリングせず、ISRがセットする
 * フラグ（g_rxComplete / g_cmdError）のみを監視する。
 * コマンド判定（dispatch）もISR内の WAIT_CMD 遷移時に行う。
 */

#include "i2c_slave.h"
#include "uart_print.h"

/*--------------------------------------------------------------
 * 内部バッファ
 * 256バイト = 128words
 *--------------------------------------------------------------*/
static uint16_t s_rxBuf[I2C_RX_WORDS];

/*--------------------------------------------------------------
 * 割り込み共有変数（i2c_slave.h で型定義済み）
 *
 * s_rxState    : ISR内の受信ステート（CMD/LEN/DATA）
 * s_cmd        : 受信したコマンドバイト
 * s_len        : 受信したLENバイト
 * g_rxExpected : 今回受信すべきバイト数（LEN確定時にISRがセット）
 * g_rxByteCount: 受信済みバイト数（ISRがインクリメント）
 * g_rxComplete : 1パケット受信完了フラグ（ISRがtrueにする）
 * g_cmdError   : 未知のCMDを受信した場合にtrueにする
 *--------------------------------------------------------------*/
static volatile I2cRxState s_rxState    = I2C_RXSTATE_WAIT_CMD;
static volatile uint16_t   s_cmd        = 0U;
static volatile uint16_t   s_len        = 0U;
static volatile uint32_t   g_rxExpected  = 0U;
static volatile uint32_t   g_rxByteCount = 0U;
static volatile bool       g_rxComplete  = false;
static volatile bool       g_cmdError    = false;

/*--------------------------------------------------------------
 * ステータス応答バッファ（「先入れ方式」）
 *
 * s_statusFrame[0] = STATUSコード
 * s_statusFrame[1] = CRC-8(STATUSコード)
 *
 * I2C_SetAppStatus() が呼ばれた瞬間にここへ組み立てておき、
 * I2C_AddrISR（AAS割り込み／READ要求検出）では計算せず
 * このバッファをそのままTX FIFOへコピーするだけにする。
 *--------------------------------------------------------------*/
static volatile uint16_t s_statusFrame[I2C_STATUS_FRAME_LEN] =
{
    (uint16_t)I2C_APP_STATUS_APP_START, 0U
};

/*--------------------------------------------------------------
 * CRC-8 計算（poly=0x07, init=0x00, 反転なし／一般的な"CRC-8"）
 * 応答フレームはごく短い（1byte）ため、テーブルは使わず
 * ビット単位計算とする。
 *--------------------------------------------------------------*/
static uint8_t I2C_Crc8(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0x00U;
    uint32_t i, bit;

    for(i = 0U; i < len; i++)
    {
        crc ^= data[i];
        for(bit = 0U; bit < 8U; bit++)
        {
            if((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1U) ^ 0x07U);
            }
            else
            {
                crc = (uint8_t)(crc << 1U);
            }
        }
    }
    return crc;
}

/*--------------------------------------------------------------
 * I2C_SetAppStatus
 * アプリの状態が変化した瞬間に呼び出す。
 * READ応答フレーム [STATUS][CRC8] をこの場で組み立てて保持しておく
 * （＝先入れ）。AAS割り込み側は計算せずコピーするだけで済む。
 *
 * s_statusFrame はISR(I2C_AddrISR)からも参照されるため、
 * 更新中にAASが割り込んで半端な状態を読まれないよう、
 * 更新中はI2Cベーシック割り込みを一時的に止める。
 *--------------------------------------------------------------*/
void I2C_SetAppStatus(I2cAppStatus status)
{
    uint8_t statusByte = (uint8_t)status;
    uint8_t crc         = I2C_Crc8(&statusByte, 1U);

    I2C_disableInterrupt(I2CA_BASE, I2C_INT_ADDR_SLAVE);

    s_statusFrame[0] = (uint16_t)statusByte;
    s_statusFrame[1] = (uint16_t)crc;

    I2C_enableInterrupt(I2CA_BASE, I2C_INT_ADDR_SLAVE);
}

/*--------------------------------------------------------------
 * I2C_SlaveInit
 * I2Cスレーブ初期化 + 割り込みベクタ登録
 *--------------------------------------------------------------*/
void I2C_SlaveInit(void)
{
    //
    // 1. GPIO設定
    //
    GPIO_setPinConfig(I2C_SDA_CFG);
    GPIO_setPadConfig(I2C_SDA_PIN, GPIO_PIN_TYPE_STD | GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(I2C_SDA_PIN, GPIO_QUAL_ASYNC);

    GPIO_setPinConfig(I2C_SCL_CFG);
    GPIO_setPadConfig(I2C_SCL_PIN, GPIO_PIN_TYPE_STD | GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(I2C_SCL_PIN, GPIO_QUAL_ASYNC);

    //
    // 2. I2CA モジュール初期化
    //
    I2C_disableModule(I2CA_BASE);

    // スレーブアドレス設定
    I2C_setOwnSlaveAddress(I2CA_BASE, I2C_SLAVE_ADDRESS);

    // 8ビットデータ長
    I2C_setBitCount(I2CA_BASE, I2C_BITCOUNT_8);

    // スレーブ受信モード
    I2C_setConfig(I2CA_BASE, I2C_SLAVE_RECEIVE_MODE);

    I2C_enableFIFO(I2CA_BASE);

#if IRC_CODE
    // FIFO割り込みフラグ(RXFF)をクリア
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RXFF);

    // RX FIFO割り込みレベルを RX1（1バイト）に設定
    // 1バイト受信ごとに割り込みが発生し、ISR内のステートマシンで
    // CMD/LEN/DATAのどれを受信中かを判定して振り分ける。
    I2C_setFIFOInterruptLevel(I2CA_BASE,
                              I2C_FIFO_TX1,   // TX側は未使用のため最小値
                              I2C_FIFO_RX1);  // RX: 1バイトで割り込み

    // 有効化する割り込みソースを I2C_INT_RXFF に設定
    I2C_enableInterrupt(I2CA_BASE, I2C_INT_RXFF);

    // アドレス指定検出(AAS)割り込みを有効化。
    // RESTART+READでステータス問い合わせが来たことをここで検出する。
    // ※ I2C_INT_ADDR_SLAVE はFIFO割り込みとは別系統（ベーシック割り込み）。
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_ADDR_SLAVE);
    I2C_enableInterrupt(I2CA_BASE, I2C_INT_ADDR_SLAVE);

    // ステートマシンを初期状態に戻す
    s_rxState     = I2C_RXSTATE_WAIT_CMD;
    g_rxComplete  = false;
    g_cmdError    = false;

    // PIE割り込みベクタ登録
    // ・INT_I2CA_FIFO : FIFO割り込み（RXFF等）= Group8
    // ・INT_I2CA      : ベーシック割り込み（AAS等）= Group8
    //   ※実際のベクタ名はご使用のデバイスのinterrupt.hに合わせてください。
    Interrupt_register(INT_I2CA_FIFO, &I2C_RxISR);
    Interrupt_enable(INT_I2CA_FIFO);

    Interrupt_register(INT_I2CA, &I2C_AddrISR);
    Interrupt_enable(INT_I2CA);

    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
#else
    // ポーリングモード：割り込みは一切使用しない。
    // I2C_FwUpdate側のwhileループで直接レジスタ/FIFOステータスを監視する。
    // （AASによるステータス問い合わせ応答は、この構成では未対応。
    //   必要な場合はI2C_getStatus()のI2C_STS_ADDR_SLAVEをポーリングして
    //   同様の処理を追加してください。）
#endif

    I2C_enableModule(I2CA_BASE);

    // 起動直後のステータスを応答フレームに反映（先入れ）
    I2C_SetAppStatus(I2C_APP_STATUS_APP_START);
}

/*--------------------------------------------------------------
 * I2C_RxISR
 * I2C RX FIFO割り込みハンドラ（1バイト/回）
 *
 * ステートマシン:
 *   WAIT_CMD -> (CMDバイト受信) 対応コマンドなら WAIT_LEN へ、
 *               未知のCMDなら g_cmdError=true にして WAIT_CMD に留まる
 *   WAIT_LEN -> (LENバイト受信) g_rxExpected を確定し RX_DATA へ
 *               (LEN==0x00 は 256byte フル受信を意味する)
 *   RX_DATA  -> データを2バイト単位でwordへ格納。
 *               g_rxByteCount が g_rxExpected に達したら
 *               g_rxComplete=true にして WAIT_CMD に戻る
 *
 * コマンドの追加方法:
 *   WAIT_CMD 分岐の switch(rxByte) に case を追加するだけで良い。
 *   コマンドごとに固定長にしたい場合はここで g_rxExpected を
 *   確定させ、WAIT_LEN をスキップして直接 RX_DATA に遷移させてもよい。
 *--------------------------------------------------------------*/
#if IRC_CODE
__interrupt void I2C_RxISR(void)
{
    uint16_t rxByte = I2C_getData(I2CA_BASE);

    switch(s_rxState)
    {
        case I2C_RXSTATE_WAIT_CMD:

            //
            // ここが「コマンドによって対応を振り分ける」分岐点。
            // 現状は I2C_CMD_FW_WRITE のみだが、case を追加すれば
            // コマンドごとに別処理（別ステートや固定長）へ拡張できる。
            //
            switch(rxByte)
            {
                case I2C_CMD_FW_WRITE:
                    s_cmd      = rxByte;
                    g_cmdError = false;
                    s_rxState  = I2C_RXSTATE_WAIT_LEN;
                    break;

                case I2C_CMD_STATUS_QUERY:
                    // ステータス問い合わせコマンド。
                    // LEN/DATAは送られてこない想定（このあとRESTART+READが
                    // 来る）ため、WAIT_LENには進めずWAIT_CMDのまま待機する。
                    // 実際の応答はI2C_AddrISR（AAS割り込み）側で完結する。
                    s_cmd      = rxByte;
                    g_cmdError = false;
                    s_rxState  = I2C_RXSTATE_WAIT_CMD;
                    break;

                default:
                    // 未知のコマンド：エラーフラグを立てて
                    // WAIT_CMDに留まる（次バイトを新たなCMDとして扱う）
                    g_cmdError = true;
                    s_rxState  = I2C_RXSTATE_WAIT_CMD;
                    break;
            }
            break;

        case I2C_RXSTATE_WAIT_LEN:

            s_len         = rxByte;
            g_rxExpected  = (s_len == I2C_DATA_FULL) ? I2C_RX_FULL_SIZE
                                                      : (uint32_t)s_len;
            g_rxByteCount = 0U;
            s_rxState     = I2C_RXSTATE_RX_DATA;
            break;

        case I2C_RXSTATE_RX_DATA:

            if(g_rxByteCount < g_rxExpected)
            {
                // 2バイトを1wordに変換（リトルエンディアン）
                if(g_rxByteCount % 2U == 0U)
                {
                    s_rxBuf[g_rxByteCount / 2U] = rxByte;
                }
                else
                {
                    s_rxBuf[g_rxByteCount / 2U] |= (uint16_t)(rxByte << 8U);
                }
                g_rxByteCount++;
            }

            if(g_rxByteCount >= g_rxExpected)
            {
                g_rxComplete = true;
                s_rxState    = I2C_RXSTATE_WAIT_CMD;
            }
            break;

        default:
            // ここに来ることは想定していないが、念のため復帰させる
            s_rxState = I2C_RXSTATE_WAIT_CMD;
            break;
    }

    //
    // 割り込みフラグクリア → PIEアクノリッジ の順で必ず実施する
    //
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RXFF);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
}

/*--------------------------------------------------------------
 * I2C_AddrISR
 * アドレス指定検出(AAS)割り込みハンドラ。
 * RX FIFO割り込みとは別系統（ベーシック割り込み／INT_I2CA）。
 *
 * マスターがSTART/RESTARTでこのスレーブをアドレス指定するたびに
 * 発生する。I2C_STS_SLAVE_DIR を見て方向を判定する。
 *   ・方向=送信（マスターがREAD要求） :
 *       s_statusFrame（先入れ済みのSTATUS+CRC8）をそのまま
 *       TX FIFOへ積んでスレーブ送信モードへ切り替える。
 *       モジュールはデータが積まれるまでSCLをストレッチするため、
 *       ここでの応答遅延がバス上のタイミング破綻に直結することはない。
 *   ・方向=受信（マスターがWRITE要求） :
 *       スレーブ受信モードに戻す（通常のCMD/LEN/DATA受信フロー）。
 *--------------------------------------------------------------*/
__interrupt void I2C_AddrISR(void)
{
    uint16_t status = I2C_getStatus(I2CA_BASE);

    if((status & I2C_STS_ADDR_SLAVE) != 0U)
    {
        if((status & I2C_STS_SLAVE_DIR) != 0U)
        {
            // マスターがREAD要求 → 先入れ済みフレームをそのまま送出
            I2C_setConfig(I2CA_BASE, I2C_SLAVE_TRANSMIT_MODE);
            I2C_putData(I2CA_BASE, s_statusFrame[0]);
            I2C_putData(I2CA_BASE, s_statusFrame[1]);

            // このコマンドはここで完結。次のCMD受信に備えてリセット。
            s_rxState = I2C_RXSTATE_WAIT_CMD;
        }
        else
        {
            // マスターがWRITE要求 → 通常の受信フローに戻す
            I2C_setConfig(I2CA_BASE, I2C_SLAVE_RECEIVE_MODE);
        }
    }

    I2C_clearStatus(I2CA_BASE, I2C_STS_ADDR_SLAVE);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
}
#endif /* IRC_CODE */

#if !IRC_CODE
/*--------------------------------------------------------------
 * I2C_PollReadByte
 * 1バイト受信されるまでポーリングして返す共通ヘルパー。
 *--------------------------------------------------------------*/
static uint16_t I2C_PollReadByte(void)
{
    while(!(I2C_getStatus(I2CA_BASE) & I2C_STS_RX_DATA_RDY))
    {
        // ポーリング待ち
    }
    return I2C_getData(I2CA_BASE);
}

/*--------------------------------------------------------------
 * I2C_PollWaitCmd17
 * ポーリング方式でCMDバイトを取得する（IRC_CODE=0時の代替実装）。
 *
 *   ・受信バイトが I2C_CMD_FW_WRITE (0x17) ならそのまま返す。
 *   ・0x17以外を受信した場合はそのフレーム全体を無視する。
 *     マスターがそのままSTOPを送ってくる想定のため、
 *     STOPコンディションが検出されるまで後続バイトを読み捨て続け、
 *     STOP検出後に再び次のCMDバイト待ちに戻る。
 *
 * 戻り値: 常に I2C_CMD_FW_WRITE (0x17)
 *         （呼び出し元はこの後LEN/DATAの受信処理へ進めばよい）
 *--------------------------------------------------------------*/
static uint16_t I2C_PollWaitCmd17(void)
{
    uint16_t rxByte;

    while(1)
    {
        rxByte = I2C_PollReadByte();

        if(rxByte == I2C_CMD_FW_WRITE)
        {
            return rxByte; // 目的のCMDを受信
        }

        //
        // 0x17以外 → このフレームは無視し、STOPまで読み捨てる
        //
        UART_printStr("Unexpected CMD, discard until STOP: ");
        UART_printHex(rxByte);
        UART_printStr("\r\n");

        while(!(I2C_getStatus(I2CA_BASE) & I2C_STS_STOP_CONDITION))
        {
            if(I2C_getStatus(I2CA_BASE) & I2C_STS_RX_DATA_RDY)
            {
                (void)I2C_getData(I2CA_BASE); // 読み捨て
            }
        }

        // STOPフラグをクリアして次のCMD待ちに戻る
        I2C_clearStatus(I2CA_BASE, I2C_STS_STOP_CONDITION);
    }
}

/*--------------------------------------------------------------
 * I2C_PollReceiveFrame
 * CMD(0x17)受信後、LEN→DATA本体をポーリングで受信する。
 * ISR版のWAIT_LEN/RX_DATAステートと全く同じ処理
 * （s_len / g_rxExpected / s_rxBuf への格納、LEN==0x00は256byte、
 *   2byteずつリトルエンディアンでword化）をポーリングで行う。
 *
 * 完了時点で s_len / g_rxExpected / s_rxBuf は
 * I2C_HandleFwWrite() がそのまま使える状態になっている。
 *--------------------------------------------------------------*/
static void I2C_PollReceiveFrame(void)
{
    uint16_t rxByte;
    uint32_t i;

    // LENバイト受信
    rxByte       = I2C_PollReadByte();
    s_len        = rxByte;
    g_rxExpected = (s_len == I2C_DATA_FULL) ? I2C_RX_FULL_SIZE
                                             : (uint32_t)s_len;

    // DATA本体受信
    for(i = 0U; i < g_rxExpected; i++)
    {
        rxByte = I2C_PollReadByte();

        if(i % 2U == 0U)
        {
            s_rxBuf[i / 2U] = rxByte;
        }
        else
        {
            s_rxBuf[i / 2U] |= (uint16_t)(rxByte << 8U);
        }
    }
}
#endif /* !IRC_CODE */

/*--------------------------------------------------------------
 * I2C_HandleFwWrite
 * CMD=I2C_CMD_FW_WRITE のパケットを1個処理する。
 *
 * destAddr はFW書き込みシーケンス中だけ進める必要があるため、
 * 呼び出し元（I2C_FwUpdate）でstatic的に保持し、ポインタで渡す。
 *
 * 戻り値: true  = このシーケンスが完了した（LEN != 0x00 受信）
 *         false = まだ続く（LEN == 0x00、次パケット待ち）
 *--------------------------------------------------------------*/
static bool I2C_HandleFwWrite(uint32_t *destAddr, FlashCtrlResult *outResult)
{
    uint32_t actualWords = g_rxExpected / 2U;

    UART_printStr("Receiving ");
    UART_printHex((uint16_t)g_rxExpected);
    UART_printStr(" bytes -> addr=");
    UART_printHex((uint16_t)(*destAddr));
    UART_printStr("\r\n");

    *outResult = Flash_WriteData(*destAddr, s_rxBuf, actualWords);
    if(*outResult != FLASH_CTRL_OK)
    {
        UART_printStr("Flash Write FAIL\r\n");
        return true; // シーケンス打ち切り（エラー）
    }
    UART_printStr("Write OK\r\n");

    if(s_len != I2C_DATA_FULL)
    {
        UART_printStr("=== FW Update Complete ===\r\n");
        return true; // 最終パケット受信、シーケンス完了
    }

    *destAddr += I2C_RX_WORDS; // 次の書き込みアドレスへ進める
    return false;
}

/*--------------------------------------------------------------
 * ※ CMD=I2C_CMD_STATUS_QUERY (0x20) はここには来ない。
 * このコマンドはWRITEフェーズ(CMDバイトのみ)とその後のRESTART+READが
 * セットであり、応答はI2C_AddrISR側で完結するため、
 * g_rxComplete は立たず、このディスパッチループには到達しない。
 *--------------------------------------------------------------*/

/*--------------------------------------------------------------
 * I2C_FwUpdate
 * コマンド振り分けメインループ
 *
 * パケット構造:
 *   [CMD][LEN: 0x00=256byte / !0x00=最終パケット][DATA...]
 *
 * IRC_CODE=1: 完全割り込み駆動。ISRがセットする
 *             g_rxComplete / g_cmdError のみを監視する。
 * IRC_CODE=0: ポーリング駆動。I2C_PollWaitCmd17() / I2C_PollReceiveFrame()
 *             で直接レジスタ/FIFOステータスを監視して受信する。
 *--------------------------------------------------------------*/
#if IRC_CODE
I2cSlaveResult I2C_FwUpdate(void)
{
    uint32_t        destAddr = FLASH_ERASE_START_ADDR;
    FlashCtrlResult flashResult;
    bool            fwSeqDone;

    UART_printStr("=== Command Loop Start ===\r\n");
    I2C_SetAppStatus(I2C_APP_STATUS_IDLE);

    while(1)
    {
        //
        // 1. ISRがCMD/LEN/DATAを受信し終える(g_rxComplete)
        //    または未知CMDを検出する(g_cmdError)まで待機
        //
        while(!g_rxComplete && !g_cmdError)
        {
            // 何もしない：完全に割り込み駆動
        }

        if(g_cmdError)
        {
            UART_printStr("Invalid CMD\r\n");
            g_cmdError = false;
            continue; // このコマンドは無視して次のCMD受信を待つ
        }

        g_rxComplete = false;

        //
        // 2. 受信済みコマンド(s_cmd)に応じて処理を振り分ける
        //
        switch(s_cmd)
        {
            case I2C_CMD_FW_WRITE:
                I2C_SetAppStatus(I2C_APP_STATUS_WRITING);
                fwSeqDone = I2C_HandleFwWrite(&destAddr, &flashResult);
                if(fwSeqDone)
                {
                    if(flashResult != FLASH_CTRL_OK)
                    {
                        I2C_SetAppStatus(I2C_APP_STATUS_WRITE_FAIL);
                        return I2C_SLAVE_FLASH_WRITE_FAIL;
                    }
                    I2C_SetAppStatus(I2C_APP_STATUS_WRITE_DONE);
                    return I2C_SLAVE_OK;
                }
                // fwSeqDone==false なら継続受信（次パケット待ち）
                I2C_SetAppStatus(I2C_APP_STATUS_IDLE);
                break;

            default:
                // ここには来ない想定（ISRで既知コマンドのみ通す）
                break;
        }
    }
}
#else
I2cSlaveResult I2C_FwUpdate(void)
{
    uint32_t        destAddr = FLASH_ERASE_START_ADDR;
    FlashCtrlResult flashResult;
    bool            fwSeqDone;

    UART_printStr("=== Command Loop Start (Polling) ===\r\n");
    I2C_SetAppStatus(I2C_APP_STATUS_IDLE);

    while(1)
    {
        // 1. 0x17が来るまで待つ（それ以外はSTOPまで読み捨てて無視）
        (void)I2C_PollWaitCmd17();

        // 2. LEN + DATA本体を受信（s_len/g_rxExpected/s_rxBufが確定する）
        I2C_PollReceiveFrame();

        // 3. Flashへ書き込み
        I2C_SetAppStatus(I2C_APP_STATUS_WRITING);
        fwSeqDone = I2C_HandleFwWrite(&destAddr, &flashResult);
        if(fwSeqDone)
        {
            if(flashResult != FLASH_CTRL_OK)
            {
                I2C_SetAppStatus(I2C_APP_STATUS_WRITE_FAIL);
                return I2C_SLAVE_FLASH_WRITE_FAIL;
            }
            I2C_SetAppStatus(I2C_APP_STATUS_WRITE_DONE);
            return I2C_SLAVE_OK;
        }

        // fwSeqDone==false なら次パケット(0x17)待ちへ戻る
        I2C_SetAppStatus(I2C_APP_STATUS_IDLE);
    }
}
#endif /* IRC_CODE */

/*--------------------------------------------------------------
 * I2C_GetRxBuffer
 * 受信バッファへのポインタを返す（flash_test.c のベリファイ用）
 *--------------------------------------------------------------*/
const uint16_t* I2C_GetRxBuffer(void)
{
    return s_rxBuf;
}
