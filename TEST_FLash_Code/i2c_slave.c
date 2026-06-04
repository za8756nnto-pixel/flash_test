/*
 * i2c_slave.c
 *
 * I2Cスレーブ受信・Flash書き込み制御
 */

#include "i2c_slave.h"
#include "uart_print.h"

// 受信バッファ（256バイト = 128words）
static uint16_t s_rxBuf[I2C_RX_WORDS];

//
// I2C_SlaveInit
// I2Cスレーブ初期化
//
void I2C_SlaveInit(void)
{
    // GPIO設定
    GPIO_setPinConfig(I2C_SDA_CFG);
    GPIO_setPadConfig(I2C_SDA_PIN, GPIO_PIN_TYPE_STD | GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(I2C_SDA_PIN, GPIO_QUAL_ASYNC);

    GPIO_setPinConfig(I2C_SCL_CFG);
    GPIO_setPadConfig(I2C_SCL_PIN, GPIO_PIN_TYPE_STD | GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(I2C_SCL_PIN, GPIO_QUAL_ASYNC);

    // I2CA初期化
    I2C_disableModule(I2CA_BASE);

    I2C_setOwnSlaveAddress(I2CA_BASE, I2C_SLAVE_ADDRESS);
    I2C_setBitCount(I2CA_BASE, I2C_BITCOUNT_8);

    I2C_setConfig(I2CA_BASE, I2C_SLAVE_RECEIVE_MODE);

    I2C_enableFIFO(I2CA_BASE);
    I2C_clearInterruptStatus(I2CA_BASE, I2C_INT_RX_DATA_RDY);

    I2C_setFIFOInterruptLevel(I2CA_BASE,
                              I2C_FIFO_TX1,
                              I2C_FIFO_RX1);

    I2C_enableInterrupt(I2CA_BASE, I2C_INT_RX_DATA_RDY);

    I2C_enableModule(I2CA_BASE);
}

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
