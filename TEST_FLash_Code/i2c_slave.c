/*
 * i2c_slave.c
 *
 * I2Cスレーブ受信・Flash書き込み制御
 */

#include "i2c_slave.h"
#include "uart_print.h"

/*--------------------------------------------------------------
 * 内部バッファ
 * 256バイト = 128words
 *--------------------------------------------------------------*/
static uint16_t s_rxBuf[I2C_RX_WORDS];

/*--------------------------------------------------------------
 * 割り込み共有変数（i2c_slave.h で extern 宣言済み）
 *
 * g_rxExpected : 今回受信すべきバイト数（割り込みハンドラの終了判定に使う）
 * g_rxByteCount: 受信済みバイト数（割り込みハンドラがインクリメント）
 * g_rxComplete : 受信完了フラグ（割り込みハンドラが true にする）
 *--------------------------------------------------------------*/
static volatile uint32_t g_rxExpected  = 0U;
static volatile uint32_t g_rxByteCount = 0U;
static volatile bool     g_rxComplete  = false;

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
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RX_DATA_RDY);
    
#if IRC_CODE
    //    RX FIFO割り込みレベルを RX4（4バイト）に設定する。
    //    4バイトたまるごとに割り込みが発生し、FIFOが満杯(4byte)に
    //    なる前にデータを吸い出せるため Clock Stretching を回避できる。
	 I2C_setFIFOInterruptLevel(I2CA_BASE,
                              I2C_FIFO_TX1,   // TX側は未使用のため最小値
                              I2C_FIFO_RX4);  // RX: 4バイトで割り込み
#else
    //ポーリングなので、1バイト単位でいけると思われる。
    I2C_setFIFOInterruptLevel(I2CA_BASE,
                              I2C_FIFO_TX1,
                              I2C_FIFO_RX1);
	
#endif	
	
	
    I2C_enableInterrupt(I2CA_BASE, I2C_INT_RX_DATA_RDY);

#if IRC_CODE
    // PIE割り込みベクタ登録
    //    INT_I2CA_FIFO = Group8 の割り込み
    //
    Interrupt_register(INT_I2CA_FIFO, &I2C_RxISR);
    Interrupt_enable(INT_I2CA_FIFO);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
#endif

    I2C_enableModule(I2CA_BASE);
}

#if IRC_CODE
/*--------------------------------------------------------------
 * I2C_RxISR
 * I2C RX FIFO割り込みハンドラ
 *
 * 呼び出しタイミング:
 *   RX FIFOに4バイト以上たまったとき（I2C_FIFO_RX4設定時）
 *
 * 処理:
 *   FIFOにあるデータを全て吸い出し、s_rxBuf に格納する。
 *   g_rxExpected バイト受信完了で g_rxComplete = true にする。
 *--------------------------------------------------------------*/
__interrupt void I2C_RxISR(void)
{
    uint16_t rxByte;

    //
    // FIFOにあるデータを全て吸い出す
    // （割り込み1回につき複数バイト処理することで取りこぼしを防ぐ）
    //
    while(I2C_getRxFIFOStatus(I2CA_BASE) != I2C_FIFO_RXEMPTY)
    {
        rxByte = I2C_getData(I2CA_BASE);

        if(g_rxByteCount < g_rxExpected)
        {
            // 2バイトを1wordに変換（リトルエンディアン）
            // 偶数バイト目 → 下位バイト
            // 奇数バイト目 → 上位バイト
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
    }

    //
    // 受信すべきバイト数に達したら完了フラグを立てる
    //
    if(g_rxByteCount >= g_rxExpected)
    {
        g_rxComplete = true;
    }

    //
    // 割り込みフラグクリア → PIEアクノリッジ の順で必ず実施する
    //
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RX_DATA_RDY);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP8);
}

/*--------------------------------------------------------------
 * I2C_FwUpdate
 * FW書き込みメインループ
 *
 * パケット構造:
 *   [CMD: 0x17][LEN: 0x00=256byte / !0x00=最終パケット][DATA...]
 *
 * LEN == 0x00 のパケットを受信し続け、
 * LEN != 0x00 の最終パケットを受信したら終了する。
 *--------------------------------------------------------------*/
I2cSlaveResult I2C_FwUpdate(void)
{
    uint32_t       destAddr = FLASH_ERASE_START_ADDR;
    uint16_t       cmd;
    uint16_t       len;
    uint32_t       rxSize;
    uint32_t       actualWords;
    uint32_t       timeoutCount;
    FlashCtrlResult flashResult;

    UART_printStr("=== FW Update Start ===\r\n");

    while(1)
    {
        /*----------------------------------------------------------
         * 1. CMDバイト受信待ち（ポーリング）
         *    CMD/LEN はパケット先頭の制御バイトのため、
         *    確実に取得するためにポーリングで処理する。
         *----------------------------------------------------------*/
        timeoutCount = 0U;
        while(I2C_getRxFIFOStatus(I2CA_BASE) == I2C_FIFO_RXEMPTY)
        {
            timeoutCount++;
            if(timeoutCount >= (DEVICE_SYSCLK_FREQ / 1U))
            {
                UART_printStr("Timeout waiting CMD\r\n");
                return I2C_SLAVE_TIMEOUT;
            }
        }
        cmd = I2C_getData(I2CA_BASE);

        if(cmd != I2C_CMD_FW_WRITE)
        {
            UART_printStr("Invalid CMD\r\n");
            return I2C_SLAVE_INVALID_CMD;
        }

        /*----------------------------------------------------------
         * 2. LENバイト受信待ち（ポーリング）
         *----------------------------------------------------------*/
        timeoutCount = 0U;
        while(I2C_getRxFIFOStatus(I2CA_BASE) == I2C_FIFO_RXEMPTY)
        {
            timeoutCount++;
            if(timeoutCount >= (DEVICE_SYSCLK_FREQ / 1U))
            {
                UART_printStr("Timeout waiting LEN\r\n");
                return I2C_SLAVE_TIMEOUT;
            }
        }
        len = I2C_getData(I2CA_BASE);

        /*----------------------------------------------------------
         * 3. DATA受信（割り込み）
         *    g_rxExpected に受信バイト数をセットして割り込みに任せる。
         *    g_rxComplete が true になるまでここで待機する。
         *----------------------------------------------------------*/
        rxSize      = (len == I2C_DATA_FULL) ? I2C_RX_FULL_SIZE : (uint32_t)len;
        actualWords = rxSize / 2U;

        UART_printStr("Receiving ");
        UART_printHex((uint16_t)rxSize);
        UART_printStr(" bytes -> addr=");
        UART_printHex((uint16_t)destAddr);
        UART_printStr("\r\n");

        // 割り込み受信の準備
        g_rxExpected  = rxSize;
        g_rxByteCount = 0U;
        g_rxComplete  = false;

        // 受信完了待ち（タイムアウト付き）
        timeoutCount = 0U;
        while(!g_rxComplete)
        {
            timeoutCount++;
            if(timeoutCount >= (DEVICE_SYSCLK_FREQ / 1U))
            {
                UART_printStr("Timeout waiting DATA\r\n");
                return I2C_SLAVE_TIMEOUT;
            }
        }

        /*----------------------------------------------------------
         * 4. Flashに書き込む
         *----------------------------------------------------------*/
        flashResult = Flash_WriteData(destAddr, s_rxBuf, actualWords);
        if(flashResult != FLASH_CTRL_OK)
        {
            UART_printStr("Flash Write FAIL\r\n");
            return I2C_SLAVE_FLASH_WRITE_FAIL;
        }
        UART_printStr("Write OK\r\n");

        /*----------------------------------------------------------
         * 5. 終端判定（LEN != 0x00 で終了）
         *----------------------------------------------------------*/
        if(len != I2C_DATA_FULL)
        {
            UART_printStr("=== FW Update Complete ===\r\n");
            break;
        }

        /*----------------------------------------------------------
         * 6. 次の書き込みアドレスへ進める
         *    256バイト = 128words
         *----------------------------------------------------------*/
        destAddr += I2C_RX_WORDS;
    }

    return I2C_SLAVE_OK;
}

/*--------------------------------------------------------------
 * I2C_GetRxBuffer
 * 受信バッファへのポインタを返す（flash_test.c のベリファイ用）
 *--------------------------------------------------------------*/
const uint16_t* I2C_GetRxBuffer(void)
{
    return s_rxBuf;
}

#else

//
// I2C_FwUpdate
// FW書き込みメインループ
// 256バイト受信ごとにFlashへ書き込む
// LEN != 0x00 の受信で終了
//
I2cSlaveResult I2C_FwUpdate(void)
{
    uint32_t destAddr = FLASH_ERASE_START_ADDR;
    uint16_t cmd;
    uint16_t len;
    uint16_t rxByte;
    uint32_t i;
    uint32_t actualWords;
    FlashCtrlResult flashResult;

    UART_printStr("=== FW Update Start ===\r\n");

    while(1)
    {
        //
        // 1. コマンドバイト受信待ち
        //
        while(I2C_getRxFIFOStatus(I2CA_BASE) == I2C_FIFO_RXEMPTY){}
        cmd = I2C_getData(I2CA_BASE);

        if(cmd != I2C_CMD_FW_WRITE)
        {
            UART_printStr("Invalid CMD\r\n");
            return I2C_SLAVE_INVALID_CMD;
        }

        //
        // 2. LENバイト受信待ち
        //
        while(I2C_getRxFIFOStatus(I2CA_BASE) == I2C_FIFO_RXEMPTY){}
        len = I2C_getData(I2CA_BASE);

        //
        // 3. データ受信
        //    len == 0x00 → 256バイト受信
        //    len != 0x00 → lenバイト受信（最終パケット）
        //
        uint32_t rxSize = (len == I2C_DATA_FULL) ? I2C_RX_FULL_SIZE : (uint32_t)len;
        actualWords = rxSize / 2U;

        UART_printStr("Receiving ");
        UART_printHex(rxSize);
        UART_printStr(" bytes -> ");
        UART_printHex(destAddr);
        UART_printStr("\r\n");

        for(i = 0; i < rxSize; i++)
        {
            // 1バイト受信待ち
            while(I2C_getRxFIFOStatus(I2CA_BASE) == I2C_FIFO_RXEMPTY){}
            rxByte = I2C_getData(I2CA_BASE);

            // 2バイトを1wordに変換（リトルエンディアン）
            if(i % 2 == 0)
            {
                s_rxBuf[i / 2] = rxByte;
            }
            else
            {
                s_rxBuf[i / 2] |= (uint16_t)(rxByte << 8U);
            }
        }

        //
        // 4. Flashに書き込む
        //
        flashResult = Flash_WriteData(destAddr, s_rxBuf, actualWords);
        if(flashResult != FLASH_CTRL_OK)
        {
            UART_printStr("Flash Write FAIL\r\n");
            return I2C_SLAVE_FLASH_WRITE_FAIL;
        }

        UART_printStr("Write OK\r\n");

        //
        // 5. 終端判定（len != 0x00 で終了）
        //
        if(len != I2C_DATA_FULL)
        {
            UART_printStr("=== FW Update Complete ===\r\n");
            break;
        }

        //
        // 6. 次の書き込みアドレスへ進める
        //    256バイト = 128words = 0x80
        //
        destAddr += I2C_RX_WORDS;
    }

    return I2C_SLAVE_OK;
}
#endif

