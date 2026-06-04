/*
 * flash_test.c
 *
 *  Created on: 2026/06/04
 *      Author: user
 */
#include "flash_test.h"
#include "uart_print.h"
#include "flash_programming_f280015x.h"

static uint16_t testData[TEST_DATA_SIZE];

#pragma CODE_SECTION(Flash_RunTest, ".TI.ramfunc")
FlashTestResult Flash_RunTest(void)
{
    uint32_t i;
    uint32_t u32Index;
    Fapi_StatusType          status;
    Fapi_FlashStatusType     flashStatus;
    Fapi_FlashStatusWordType flashStatusWord;

    // 1. テストデータ作成
    for(i = 0; i < TEST_DATA_SIZE; i++)
    {
        testData[i] = (uint16_t)(0xA500U | (i & 0xFFU));
    }

    // 2. FSMステータスクリア（サンプルのClearFSMStatus相当）
    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
    flashStatus = Fapi_getFsmStatus();
    if(flashStatus != 0)
    {
        status = Fapi_issueAsyncCommand(Fapi_ClearStatus);
        while(Fapi_getFsmStatus() != 0){}  // ★ゼロになるまで待つ
    }

    // ★★ 3. セクター書き込み保護を解除 ★★
    // Sector8 は bit8 → CMDWEPROTA の bit8 を解除 (0xFFFFFF00→bit0-7保護, bit8-31解除)
    // Sector8(0x082000)はSector番号8なのでCMDWEPROTA bit8
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTA, 0xFFFEFFFF); // ← 変更
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTB, 0xFFFFFFFF);

    // 4. セクター消去
    status = Fapi_issueAsyncCommandWithAddress(
                 Fapi_EraseSector,
                 (uint32 *)TEST_FLASH_START_ADDR);

    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
    flashStatus = Fapi_getFsmStatus();

    UART_printStr("flashStatus after erase: ");
    UART_printHex((uint32_t)flashStatus);
    UART_printStr("\r\n");

    if(flashStatus != 3)  // ★サンプルは != 3 で判定
    {
        return FLASH_TEST_ERASE_FAIL;
    }

    // 5. 消去ベリファイ
    status = Fapi_doBlankCheck(
                 (uint32 *)TEST_FLASH_START_ADDR,
                 TEST_DATA_SIZE / 2,  // u32length = u16length / 2
                 &flashStatusWord);
    if(status != Fapi_Status_Success)
    {
        return FLASH_TEST_ERASE_FAIL;
    }

    // 6. データ書き込み（★8 wordsずつループ）
    for(i = 0, u32Index = TEST_FLASH_START_ADDR;
        i < TEST_DATA_SIZE;
        i += 8, u32Index += 8)
    {
        // ★毎回ClearFSMStatusとsetupBankSectorEnable
        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
        flashStatus = Fapi_getFsmStatus();
        if(flashStatus != 0)
        {
            Fapi_issueAsyncCommand(Fapi_ClearStatus);
            while(Fapi_getFsmStatus() != 0){}
        }

        Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTA, 0xFFFEFFFF); // ← 変更
        Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTB, 0xFFFFFFFF);

        status = Fapi_issueProgrammingCommand(
                     (uint32 *)u32Index,
                     testData + i,
                     8,
                     0, 0,
                     Fapi_AutoEccGeneration);  // ★DataOnlyではなくAutoEcc推奨

        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
        flashStatus = Fapi_getFsmStatus();
        if(flashStatus != 3)
        {
            return FLASH_TEST_PROGRAM_FAIL;
        }
    }

    // 7. 書き込みベリファイ
    uint16_t *pFlash = (uint16_t *)TEST_FLASH_START_ADDR;
    for(i = 0; i < TEST_DATA_SIZE; i++)
    {
        if(pFlash[i] != testData[i])
        {
            return FLASH_TEST_VERIFY_FAIL;
        }
    }

    return FLASH_TEST_OK;
}
