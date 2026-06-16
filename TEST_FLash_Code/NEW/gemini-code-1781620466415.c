/*
 * i2c_slave.c
 *
 * I2Cスレーブ受信・Flash書き込み制御（完全割り込み・ステートマシン版）
 */

#include "i2c_slave.h"
#include "uart_print.h"

/*--------------------------------------------------------------
 * 受信ステート（状態）の定義
 *--------------------------------------------------------------*/
typedef enum {
    I2C_STATE_WAIT_CMD = 0, // CMDバイトの受信待ち
    I2C_STATE_WAIT_LEN,     // LENバイトの受信待ち
    I2C_STATE_WAIT_DATA     // DATA本体の受信待ち
} I2cRxState;

static volatile I2cRxState g_rxState = I2C_STATE_WAIT_CMD;

/*--------------------------------------------------------------
 * 内部リングバッファ
 *--------------------------------------------------------------*/
#define I2C_RING_BUF_SIZE    (I2C_RX_WORDS * 2U) // 256words = 512bytes
static uint16_t s_rxRingBuf[I2C_RING_BUF_SIZE];
static uint32_t s_ringWriteIdx = 0U;
static uint32_t s_ringReadIdx  = 0U;

/*--------------------------------------------------------------
 * 割り込み・メインクロス共有変数
 *--------------------------------------------------------------*/
static volatile uint32_t g_rxExpected  = 0U;
static volatile uint32_t g_rxByteCount = 0U;

// メインループに「パケットが1つ完成してバッファに溜まった」ことを伝えるフラグ
static volatile bool     g_packetReady = false; 

// パケットごとの情報を保存する変数
static volatile uint16_t g_activeCmd = 0U;
static volatile uint16_t g_activeLen = 0U;

/*--------------------------------------------------------------
 * I2C_SlaveInit
 * I2Cスレーブ初期化
 *--------------------------------------------------------------*/
void I2C_SlaveInit(void)
{
    // GPIO設定
    GPIO_setPinConfig(I2C_SDA_CFG);
    GPIO_setPadConfig(I2C_SDA_PIN, GPIO_PIN_TYPE_STD | GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(I2C_SDA_PIN, GPIO_QUAL_ASYNC);

    GPIO_setPinConfig(I2C_SCL_CFG);
    GPIO_setPadConfig(I2C_SCL_PIN, GPIO_PIN_TYPE_STD | GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(I2C_SCL_PIN, GPIO_QUAL_ASYNC);

    // I2CA モジュール初期化
    I2C_disableModule(I2CA_BASE);
    I2C_setOwnSlaveAddress(I2CA_BASE, I2C_SLAVE_ADDRESS);
    I2C_setBitCount(I2CA_BASE, I2C_BITCOUNT_8);
    I2C_setConfig(I2CA_BASE, I2C_SLAVE_RECEIVE_MODE);

    // FIFOの有効化
    I2C_enableFIFO(I2CA_BASE);
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RXFF);
    
#if IRC_CODE
    // 最初はCMD(1バイト)を待つため、割り込みレベルは「1」からスタートする
    g_rxState = I2C_STATE_WAIT_CMD;
    I2C_setFIFOInterruptLevel(I2CA_BASE, I2C_FIFO_TX1, I2C_FIFO_RX1); 
#else
    I2C_setFIFOInterruptLevel(I2CA_BASE, I2C_FIFO_TX1, I2C_FIFO_RX1);
#endif	
	
    I2C_enableInterrupt(I2CA_BASE, I2C_INT_RXFF);
	
#if IRC_CODE
    Interrupt_register(INT_I2CA_FIFO, &I2C_RxISR);
    Interrupt_enable(INT_I2CA_FIFO);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
#endif

    I2C_enableModule(I2CA_BASE);

    s_ringWriteIdx = 0U;
    s_ringReadIdx  = 0U;
    g_packetReady  = false;
}

#if IRC_CODE
/*--------------------------------------------------------------
 * I2C_RxISR
 * I2C RX FIFO割り込みハンドラ（ステートマシン駆動）
 *--------------------------------------------------------------*/
__interrupt void I2C_RxISR(void)
{
    uint16_t rxByte;

    // FIFO内のデータが空になるまでループ処理
    while(I2C_getRxFIFOStatus(I2CA_BASE) != I2C_FIFO_RXEMPTY)
    {
        rxByte = I2C_getData(I2CA_BASE);

        switch(g_rxState)
        {
            case I2C_STATE_WAIT_CMD:
                g_activeCmd = rxByte;
                // CMDを受信したら、次はLEN(1バイト)を待つモードに移行
                g_rxState = I2C_STATE_WAIT_LEN;
                break;

            case I2C_STATE_WAIT_LEN:
                g_activeLen = rxByte;
                
                // DATA受信に必要なバイト数を計算
                uint32_t rxSize = (g_activeLen == I2C_DATA_FULL) ? I2C_RX_FULL_SIZE : (uint32_t)g_activeLen;
                g_rxExpected  = rxSize;
                g_rxByteCount = 0U;

                // 次はDATA本体を待つモードに移行
                g_rxState = I2C_STATE_WAIT_DATA;

                // F2800157の上限(4バイト)に合わせて、ここから効率よく4バイトずつ割り込みを入れる
                // もし残りの受信サイズが4バイト以上あれば、割り込みレベルを4に変更する
                if(g_rxExpected >= 4U)
                {
                    I2C_setFIFOInterruptLevel(I2CA_BASE, I2C_FIFO_TX1, I2C_FIFO_RX4);
                }
                else
                {
                    I2C_setFIFOInterruptLevel(I2CA_BASE, I2C_FIFO_TX1, (I2C_RxFIFOLevel)g_rxExpected);
                }
                break;

            case I2C_STATE_WAIT_DATA:
                if(g_rxByteCount < g_rxExpected)
                {
                    // リングバッファの格納位置（word単位）を計算
                    uint32_t currentWordIdx = (s_ringWriteIdx + (g_rxByteCount / 2U)) % I2C_RING_BUF_SIZE;

                    if(g_rxByteCount % 2U == 0U)
                    {
                        s_rxRingBuf[currentWordIdx] = rxByte;
                    }
                    else
                    {
                        s_rxRingBuf[currentWordIdx] |= (uint16_t)(rxByte << 8U);
                    }
                    g_rxByteCount++;
                }

                // データの受信途中で、残りが4バイト未満になった場合の動的トリガー調整
                uint32_t rem = g_rxExpected - g_rxByteCount;
                if((rem > 0U) && (rem < 4U))
                {
                    I2C_setFIFOInterruptLevel(I2CA_BASE, I2C_FIFO_TX1, (I2C_RxFIFOLevel)rem);
                }

                // パケットのすべてのDATAを回収し終えた場合
                if(g_rxByteCount >= g_rxExpected)
                {
                    // メインループに処理を依頼
                    g_packetReady = true;

                    // 次のパケット格納用にリングバッファの進捗を確定させる
                    s_ringWriteIdx = (s_ringWriteIdx + (g_rxExpected / 2U)) % I2C_RING_BUF_SIZE;

                    // 次の新しいパケットの「CMD」を待つため、ステートと割り込みレベルをリセット
                    g_rxState = I2C_STATE_WAIT_CMD;
                    I2C_setFIFOInterruptLevel(I2CA_BASE, I2C_FIFO_TX1, I2C_FIFO_RX1);
                }
                break;

            default:
                g_rxState = I2C_STATE_WAIT_CMD;
                I2C_setFIFOInterruptLevel(I2CA_BASE, I2C_FIFO_TX1, I2C_FIFO_RX1);
                break;
        }
    }

    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RXFF);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
}

/*--------------------------------------------------------------
 * I2C_FwUpdate
 * FW書き込みメインループ（完全非同期イベント待ち）
 *--------------------------------------------------------------*/
I2cSlaveResult I2C_FwUpdate(void)
{
    uint32_t       destAddr = FLASH_ERASE_START_ADDR;
    uint32_t       actualWords;
    uint32_t       timeoutCount;
    FlashCtrlResult flashResult;
    static uint16_t s_flashWriteWorkBuf[I2C_RX_WORDS]; 

    UART_printStr("=== FW Update Start (Fully-Interrupt Driven) ===\r\n");

    while(1)
    {
        /*----------------------------------------------------------
         * 1. 割り込みが「CMD + LEN + DATA」をすべて回収し終えるのを待つ
         *----------------------------------------------------------*/
        timeoutCount = 0U;
        while(!g_packetReady)
        {
            timeoutCount++;
            // 約1秒のタイムアウト監視
            if(timeoutCount >= (DEVICE_SYSCLK_FREQ / 1U))
            {
                UART_printStr("Timeout waiting Packet\r\n");
                return I2C_SLAVE_TIMEOUT;
            }
        }

        /*----------------------------------------------------------
         * 2. 割り込みが受信した CMD と LEN の検証
         *----------------------------------------------------------*/
        if(g_activeCmd != I2C_CMD_FW_WRITE)
        {
            UART_printStr("Invalid CMD\r\n");
            return I2C_SLAVE_INVALID_CMD;
        }

        uint32_t rxSize = (g_activeLen == I2C_DATA_FULL) ? I2C_RX_FULL_SIZE : (uint32_t)g_activeLen;
        actualWords = rxSize / 2U;

        UART_printStr("Processing ");
        UART_printHex((uint16_t)rxSize);
        UART_printStr(" bytes from RingBuffer...\r\n");

        /*----------------------------------------------------------
         * 3. リングバッファからFlash用ワークバッファへ安全にコピー
         *----------------------------------------------------------*/
        uint32_t i;
        for(i = 0; i < actualWords; i++)
        {
            s_flashWriteWorkBuf[i] = s_rxRingBuf[(s_ringReadIdx + i) % I2C_RING_BUF_SIZE];
        }
        s_ringReadIdx = (s_ringReadIdx + actualWords) % I2C_RING_BUF_SIZE;

        // 次のパケットを受信できるようにフラグをクリア
        g_packetReady = false;

        /*----------------------------------------------------------
         * 4. Flashに書き込む
         * この最中に次のパケットのCMD/LEN/DATAが来ても、割り込みが裏で
         * リングバッファに自動格納してくれるため、絶対にこぼれません。
         *----------------------------------------------------------*/
        UART_printStr("Writing to Flash -> addr=");
        UART_printHex((uint16_t)destAddr);
        UART_printStr("\r\n");

        flashResult = Flash_WriteData(destAddr, s_flashWriteWorkBuf, actualWords);
        if(flashResult != FLASH_CTRL_OK)
        {
            UART_printStr("Flash Write FAIL\r\n");
            return I2C_SLAVE_FLASH_WRITE_FAIL;
        }
        UART_printStr("Write OK\r\n");

        /*----------------------------------------------------------
         * 5. 終端判定
         *----------------------------------------------------------*/
         // 割り込みがキャッチしたLENをもとに判定
        if(g_activeLen != I2C_DATA_FULL)
        {
            UART_printStr("=== FW Update Complete ===\r\n");
            break;
        }

        destAddr += I2C_RX_WORDS;
    }

    return I2C_SLAVE_OK;
}

const uint16_t* I2C_GetRxBuffer(void)
{
    return s_rxRingBuf;
}
#endif