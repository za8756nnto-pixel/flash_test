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

// I2C_CMD_STATUS_REQ の応答値。main()やFW書き込み処理から更新する。
static volatile I2cAppState s_appState = I2C_APP_STATE_WAITING_FOR_WRITE;

void I2C_SetAppState(I2cAppState state)
{
    s_appState = state;
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

    // FIFO割り込みフラグ(RXFF/TXFF)をクリア
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RXFF | I2C_INT_TXFF);

    // RX FIFO割り込みレベルを RX1（1バイト）に設定
    // 1バイト受信ごとに割り込みが発生し、ISR内のステートマシンで
    // CMD/LEN/DATAのどれを受信中かを判定して振り分ける。
    // TX側は I2C_CMD_STATUS_REQ (RESTART+READ) 応答用に TX0
    // （FIFO空＝送信済み）を割り込み条件に設定する。
    // ※ I2C_FIFO_TX0 が driverlib 上の「空」判定に対応するかは
    //   使用中の driverlib バージョンのenum定義を確認してください。
    I2C_setFIFOInterruptLevel(I2CA_BASE,
                              I2C_FIFO_TX0,   // TX: 空になったら割り込み
                              I2C_FIFO_RX1);  // RX: 1バイトで割り込み

    // 有効化する割り込みソースを RXFF/TXFF 両方に設定
    // （両方とも同一の INT_I2CA_FIFO ベクタで受ける）
    I2C_enableInterrupt(I2CA_BASE, I2C_INT_RXFF | I2C_INT_TXFF);

    // ステートマシンを初期状態に戻す
    s_rxState     = I2C_RXSTATE_WAIT_CMD;
    g_rxComplete  = false;
    g_cmdError    = false;

    // PIE割り込みベクタ登録（INT_I2CA_FIFO = Group8）
    Interrupt_register(INT_I2CA_FIFO, &I2C_RxISR);
    Interrupt_enable(INT_I2CA_FIFO);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);

    I2C_enableModule(I2CA_BASE);
}

/*--------------------------------------------------------------
 * I2C_RxISR
 * I2C FIFO割り込みハンドラ（RX/TX共有ベクタ、1バイト/回）
 *
 * RX側ステートマシン:
 *   WAIT_CMD -> (CMDバイト受信)
 *       I2C_CMD_FW_WRITE     : WAIT_LENへ（従来通り[LEN][DATA]が続く）
 *       I2C_CMD_STATUS_REQ   : LEN/DATAなし。応答バイトをTXへ
 *                               先出し（プリステージ）してWAIT_CMD維持。
 *                               マスタはSTOPを打たずRESTART+READで
 *                               この値を読み出す想定。
 *       未知CMD               : g_cmdError=true
 *   WAIT_LEN -> (LENバイト受信) g_rxExpected確定、RX_DATAへ
 *   RX_DATA  -> データ格納、完了で g_rxComplete=true、WAIT_CMDへ
 *
 * TX側:
 *   TXFFイベント（FIFOが空＝送信済み）を検出したら、
 *   念のため同じステータス値を再度プリステージしておく
 *   （マスタが複数バイト読もうとするケースへの保険）。
 *--------------------------------------------------------------*/
__interrupt void I2C_RxISR(void)
{
    uint16_t intSrc = I2C_getInterruptStatus(I2CA_BASE);

    //----------------------------------------------------------
    // RX FIFOイベント
    //----------------------------------------------------------
    if(intSrc & I2C_INT_RXFF)
    {
        uint16_t rxByte = I2C_getData(I2CA_BASE);

        switch(s_rxState)
        {
            case I2C_RXSTATE_WAIT_CMD:

                //
                // ここが「コマンドによって対応を振り分ける」分岐点。
                //
                switch(rxByte)
                {
                    case I2C_CMD_FW_WRITE:
                        // [CMD][LEN][DATA] 形式。WAIT_LENへ進める。
                        s_cmd      = rxByte;
                        g_cmdError = false;
                        s_rxState  = I2C_RXSTATE_WAIT_LEN;
                        break;

                    case I2C_CMD_STATUS_REQ:
                        //
                        // LEN/DATAは続かない。マスタはこの直後に
                        // STOPなしRESTART+READで応答を読みに来るため、
                        // ここで即座にTXレジスタへ応答値を
                        // 先出し（プリステージ）しておく。
                        //
                        I2C_putData(I2CA_BASE, (uint16_t)s_appState);
                        s_cmd        = rxByte;
                        g_cmdError   = false;
                        g_rxComplete = true;   // メインループへログ通知のみ
                        s_rxState    = I2C_RXSTATE_WAIT_CMD; // 次CMD待ちのまま
                        break;

                    default:
                        // 未知のコマンド：エラーフラグを立てて
                        // WAIT_CMDに留まる（次バイトを新CMDとして扱う）
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

        I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RXFF);
    }

    //----------------------------------------------------------
    // TX FIFOイベント（STATUS_REQ応答の送信完了）
    //----------------------------------------------------------
    if(intSrc & I2C_INT_TXFF)
    {
        //
        // 保険として同じステータス値を再度プリステージしておく。
        // マスタが1バイトだけ読んで即STOPする運用であれば
        // 実質参照されないが、複数バイト読み出しに備える。
        //
        I2C_putData(I2CA_BASE, (uint16_t)s_appState);

        I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_TXFF);
    }

    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
}

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

    I2C_SetAppState(I2C_APP_STATE_WRITING);

    UART_printStr("Receiving ");
    UART_printHex((uint16_t)g_rxExpected);
    UART_printStr(" bytes -> addr=");
    UART_printHex((uint16_t)(*destAddr));
    UART_printStr("\r\n");

    *outResult = Flash_WriteData(*destAddr, s_rxBuf, actualWords);
    if(*outResult != FLASH_CTRL_OK)
    {
        UART_printStr("Flash Write FAIL\r\n");
        I2C_SetAppState(I2C_APP_STATE_WAITING_FOR_WRITE);
        return true; // シーケンス打ち切り（エラー）
    }
    UART_printStr("Write OK\r\n");

    if(s_len != I2C_DATA_FULL)
    {
        UART_printStr("=== FW Update Complete ===\r\n");
        I2C_SetAppState(I2C_APP_STATE_WRITE_COMPLETE);
        return true; // 最終パケット受信、シーケンス完了
    }

    *destAddr += I2C_RX_WORDS; // 次の書き込みアドレスへ進める
    return false;
}

/*--------------------------------------------------------------
 * I2C_LogStatusReq
 * CMD=I2C_CMD_STATUS_REQ を受信した旨をログする。
 * 応答自体はISR内で既にTXへプリステージ済みのため、
 * ここでは通知（デバッグ用ログ）のみ行う。
 *--------------------------------------------------------------*/
static void I2C_LogStatusReq(void)
{
    UART_printStr("STATUS_REQ received, appState=");
    UART_printHex((uint16_t)s_appState);
    UART_printStr("\r\n");
}

/*--------------------------------------------------------------
 * I2C_FwUpdate
 * コマンド振り分けメインループ（完全割り込み駆動）
 *
 * パケット構造:
 *   [CMD][LEN: 0x00=256byte / !0x00=最終パケット][DATA...]
 *
 * メインループはI2Cレジスタを一切ポーリングしない。
 * ISRがセットする g_rxComplete / g_cmdError のみを監視し、
 * 受信済みの s_cmd に応じて処理を振り分ける。
 * （タイムアウトが必要な場合はここにCPUタイマ等を組み合わせる）
 *--------------------------------------------------------------*/
I2cSlaveResult I2C_FwUpdate(void)
{
    uint32_t        destAddr = FLASH_ERASE_START_ADDR;
    FlashCtrlResult flashResult;
    bool            fwSeqDone;

    UART_printStr("=== Command Loop Start ===\r\n");

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
                fwSeqDone = I2C_HandleFwWrite(&destAddr, &flashResult);
                if(fwSeqDone)
                {
                    if(flashResult != FLASH_CTRL_OK)
                    {
                        return I2C_SLAVE_FLASH_WRITE_FAIL;
                    }
                    return I2C_SLAVE_OK;
                }
                // fwSeqDone==false なら継続受信（次パケット待ち）
                break;

            case I2C_CMD_STATUS_REQ:
                I2C_LogStatusReq();
                // 応答は既にISRでプリステージ済み。
                // 次のCMD受信待ちへ戻る（destAddrには触れない）
                break;

            default:
                // ここには来ない想定（ISRで既知コマンドのみ通す）
                break;
        }
    }
}

/*--------------------------------------------------------------
 * I2C_GetRxBuffer
 * 受信バッファへのポインタを返す（flash_test.c のベリファイ用）
 *--------------------------------------------------------------*/
const uint16_t* I2C_GetRxBuffer(void)
{
    return s_rxBuf;
}
