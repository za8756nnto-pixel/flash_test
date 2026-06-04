

/**
 * main.c
 */
#include "device.h"
#include "flash_test.h"
#include "uart_print.h"
#include <string.h>

void main(void)
{
    FlashTestResult result;

    Device_init();
    Device_initGPIO();

#ifdef _FLASH
    memcpy(&RamfuncsRunStart,
           &RamfuncsLoadStart,
           (size_t)&RamfuncsLoadSize);
#endif

    // ★順序重要：Flash Wait State → UART → API初期化
    Flash_initModule(FLASH0CTRL_BASE, FLASH0ECC_BASE, DEVICE_FLASH_WAITSTATES);

    UART_init();
    UART_printStr("=== Flash Test Start ===\r\n");

    // ★ CPU0_REGISTER_ADDRESS を使用
    Fapi_initializeAPI((Fapi_FmcRegistersType *)CPU0_REGISTER_ADDRESS, 120U);

    // ★ setActiveFlashBank は必ず initializeAPI の直後
    Fapi_setActiveFlashBank(Fapi_FlashBank0);

    // ★ 消去前にステータスクリア
    Fapi_issueAsyncCommand(Fapi_ClearStatus);
    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

    UART_printStr("Erasing sector...\r\n");

    result = Flash_RunTest();

    switch(result)
    {
        case FLASH_TEST_OK:
            UART_printStr("Result: OK\r\n");
            break;
        case FLASH_TEST_ERASE_FAIL:
            UART_printStr("Result: ERASE FAIL\r\n");
            break;
        case FLASH_TEST_PROGRAM_FAIL:
            UART_printStr("Result: PROGRAM FAIL\r\n");
            break;
        case FLASH_TEST_VERIFY_FAIL:
            UART_printStr("Result: VERIFY FAIL\r\n");
            break;
        default:
            break;
    }

    UART_printStr("=== Flash Test End ===\r\n");

    while(1){}
}
