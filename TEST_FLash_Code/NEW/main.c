/**
 * main.c
 */
#include "device.h"
#include "flash_ctrl.h"
#include "uart_print.h"
#include "i2c_slave.h"
#include <string.h>
#if 1
// FW書き込みモード判定
// 【後で正式な条件に変更】
// 例：0x087400のフラグを読む
static bool isFwUpdateMode(void)
{
    return Flash_IsFwUpdateFlg();
}

static void resetDevice(void)
{
    DINT;

    SysCtl_resetDevice();

    while(1) {}
}
__attribute__((noreturn))
static void jumpToApplication(void)
{
    DINT;
    IER = 0x0000;
    IFR = 0x0000;

    SysCtl_disableWatchdog();

    asm(" NOP");
    asm(" NOP");
    asm(" NOP");
    asm(" NOP");

    asm(" LB #0x082000");

    while(1){};
}

void main(void)
{
    Device_init();
    Device_initGPIO();

#ifdef _FLASH
    memcpy(&RamfuncsRunStart,
           &RamfuncsLoadStart,
           (size_t)&RamfuncsLoadSize);
#endif

    UART_init();
    UART_printStr("=== Boot Start ===\r\n");

    Interrupt_initModule();
    Interrupt_initVectorTable();

    Flash_CtrlInit();
    I2C_SlaveInit();

    EINT;
    ERTM;

    if(isFwUpdateMode())
    {
        UART_printStr("FW Update Mode\r\n");

        // 消去
        UART_printStr("Erasing...\r\n");
        FlashCtrlResult eraseResult = Flash_EraseRange(
                                          FLASH_ERASE_START_ADDR,
                                          FLASH_ERASE_END_ADDR);
        if(eraseResult != FLASH_CTRL_OK)
        {
            UART_printStr("Erase FAIL\r\n");
            while(1){}
        }
        UART_printStr("Erase OK\r\n");

        // I2C受信 → Flash書き込み
        I2cSlaveResult i2cResult = I2C_FwUpdate();
        if(i2cResult != I2C_SLAVE_OK)
        {
            UART_printStr("FW Update FAIL\r\n");
            while(1){}
        }
        //フラグクリア
        FlashCtrlResult flagResult = Flash_ClearFwUpdateFlag();
        if(flagResult != FLASH_CTRL_OK)
        {
            UART_printStr("Flag Clear FAIL\r\n");
            while(1){}
        }
        UART_printStr("Flag Clear OK\r\n");

        resetDevice();

    }
    else
    {
        UART_printStr("Normal Mode\r\n");
        jumpToApplication();
    }

    while(1){}
}
#else
void main(void)
{
    FlashCtrlResult result;

    Device_init();
    Device_initGPIO();

#ifdef _FLASH
    memcpy(&RamfuncsRunStart,
           &RamfuncsLoadStart,
           (size_t)&RamfuncsLoadSize);
#endif

    // UART初期化
    UART_init();
    UART_printStr("=== Flash Test Start ===\r\n");

    // Flash API初期化（Flash_initModule含む）
    Flash_CtrlInit();

    // 全範囲消去
    UART_printStr("Erasing...\r\n");
    result = Flash_EraseRange(FLASH_ERASE_START_ADDR, FLASH_ERASE_END_ADDR);

    if(result != FLASH_CTRL_OK)
    {
        UART_printStr("Result: ERASE FAIL\r\n");
        while(1){}
    }
    UART_printStr("Erase OK\r\n");

    // 書き込みテスト（先頭アドレスに128words=256バイト分のパターンデータ）
    uint16_t testBuf[128];
    uint32_t i;
    for(i = 0; i < 128U; i++)
    {
        testBuf[i] = (uint16_t)(0xA500U | (i & 0xFFU));
    }

    UART_printStr("Writing...\r\n");
    result = Flash_WriteData(FLASH_ERASE_START_ADDR, testBuf, 128U);

    //エラーログ出力用
    switch(result)
    {
        case FLASH_CTRL_OK:
            UART_printStr("Result: OK\r\n");
            break;
        case FLASH_CTRL_ERASE_FAIL:
            UART_printStr("Result: ERASE FAIL\r\n");
            break;
        case FLASH_CTRL_BLANKCHECK_FAIL:
            UART_printStr("Result: BLANKCHECK FAIL\r\n");
            break;
        case FLASH_CTRL_WRITE_FAIL:
            UART_printStr("Result: WRITE FAIL\r\n");
            break;
        case FLASH_CTRL_VERIFY_FAIL:
            UART_printStr("Result: VERIFY FAIL\r\n");
            break;
        case FLASH_CTRL_INVALID_ADDR:
            UART_printStr("Result: INVALID ADDR\r\n");
            break;
        default:
            break;
    }

    UART_printStr("=== Flash Test End ===\r\n");

    while(1){}
}
#endif
