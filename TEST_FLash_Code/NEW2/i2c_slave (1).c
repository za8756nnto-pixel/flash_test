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
                case I2C_CMD_OTHER_EX:
                    // どちらも [CMD][LEN][DATA] の同じフレーム形式なので
                    // ここでは共通してWAIT_LENへ進める。
                    // コマンドごとにフレーム形式自体を変えたい場合は
                    // ここでcaseを分けて別ステートに遷移させればよい。
                    s_cmd      = rxByte;
                    g_cmdError = false;
                    s_rxState  = I2C_RXSTATE_WAIT_LEN;
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
 * I2C_HandleOtherCommand
 * 【例】CMD=I2C_CMD_FW_WRITE 以外のコマンドの処理。
 * 実際の用途に応じて中身を置き換えてください
 * （s_rxBuf / g_rxExpected でデータ本体・バイト数を参照可能）。
 *--------------------------------------------------------------*/
static void I2C_HandleOtherCommand(uint16_t cmd)
{
    UART_printStr("Other CMD received: ");
    UART_printHex(cmd);
    UART_printStr(", len=");
    UART_printHex((uint16_t)g_rxExpected);
    UART_printStr("\r\n");

    // 例: 受信データを使った処理、応答送信のトリガ設定など、
    //     ここに実装する。
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

            case I2C_CMD_OTHER_EX:
                I2C_HandleOtherCommand(s_cmd);
                // このコマンドは1パケット完結として扱い、
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
