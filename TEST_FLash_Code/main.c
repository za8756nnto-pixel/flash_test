/**
 * main.c
 */
#include "device.h"
#include "flash_ctrl.h"
#include "uart_print.h"
#include <string.h>

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
