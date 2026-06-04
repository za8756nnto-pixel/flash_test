/*
 * flash_test.c
 *
 *  Created on: 2026/06/04
 *      Author: user
 */
#include "flash_test.h"
#include "uart_print.h"
#include "flash_programming_f280015x.h"

// 消去対象セクターテーブル（Sector7～Sector28）
static const FlashSectorInfo sectorTable[] =
{
    { 0x081C00U, 0x0400U },  // Sector7
    { 0x082000U, 0x0400U },  // Sector8
    { 0x082400U, 0x0400U },  // Sector9
    { 0x082800U, 0x0400U },  // Sector10
    { 0x082C00U, 0x0400U },  // Sector11
    { 0x083000U, 0x0400U },  // Sector12
    { 0x083400U, 0x0400U },  // Sector13
    { 0x083800U, 0x0400U },  // Sector14
    { 0x083C00U, 0x0400U },  // Sector15
    { 0x084000U, 0x0400U },  // Sector16
    { 0x084400U, 0x0400U },  // Sector17
    { 0x084800U, 0x0400U },  // Sector18
    { 0x084C00U, 0x0400U },  // Sector19
    { 0x085000U, 0x0400U },  // Sector20
    { 0x085400U, 0x0400U },  // Sector21
    { 0x085800U, 0x0400U },  // Sector22
    { 0x085C00U, 0x0400U },  // Sector23
    { 0x086000U, 0x0400U },  // Sector24
    { 0x086400U, 0x0400U },  // Sector25
    { 0x086800U, 0x0400U },  // Sector26
    { 0x086C00U, 0x0400U },  // Sector27
    { 0x087000U, 0x0400U },  // Sector28
};

#define NUM_SECTORS  (sizeof(sectorTable) / sizeof(sectorTable[0]))

// 書き込みバッファ（RAMに置く）
static uint16_t writeBuf[WRITE_CHUNK_SIZE];

//
// 保護解除ヘルパー
//
#pragma CODE_SECTION(Flash_ClearAndUnlock, ".TI.ramfunc")
static Fapi_StatusType Flash_ClearAndUnlock(void)
{
    Fapi_FlashStatusType flashStatus;
    Fapi_StatusType status = Fapi_Status_Success;

    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
    flashStatus = Fapi_getFsmStatus();
    if(flashStatus != 0)
    {
        status = Fapi_issueAsyncCommand(Fapi_ClearStatus);
        while(Fapi_getFsmStatus() != 0){}
    }

    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTA, 0x00000000U);
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTB, 0x00000000U);

    return status;
}

#pragma CODE_SECTION(Flash_RunTest, ".TI.ramfunc")
FlashTestResult Flash_RunTest(void)
{
    uint32_t i;
    uint32_t j;
    uint32_t u32Index;
    uint16_t u16Pattern;
    Fapi_StatusType          status;
    Fapi_FlashStatusType     flashStatus;
    Fapi_FlashStatusWordType flashStatusWord;

    //
    // 1. 全セクター消去
    //
    UART_printStr("--- Erase Start ---\r\n");

    for(i = 0; i < NUM_SECTORS; i++)
    {
        Flash_ClearAndUnlock();

        UART_printStr("Erasing: ");
        UART_printHex(sectorTable[i].startAddr);
        UART_printStr("\r\n");

        status = Fapi_issueAsyncCommandWithAddress(
                     Fapi_EraseSector,
                     (uint32 *)sectorTable[i].startAddr);

        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
        flashStatus = Fapi_getFsmStatus();

        if(flashStatus != 3)
        {
            UART_printStr("Erase FAIL: ");
            UART_printHex(sectorTable[i].startAddr);
            UART_printStr("\r\n");
            return FLASH_TEST_ERASE_FAIL;
        }

        // 消去ベリファイ
        status = Fapi_doBlankCheck(
                     (uint32 *)sectorTable[i].startAddr,
                     sectorTable[i].size / 2,
                     &flashStatusWord);

        if(status != Fapi_Status_Success)
        {
            UART_printStr("BlankCheck FAIL: ");
            UART_printHex(sectorTable[i].startAddr);
            UART_printStr("\r\n");
            return FLASH_TEST_ERASE_FAIL;
        }
    }

    UART_printStr("--- Erase OK ---\r\n");

    //
    // 2. 全セクターにパターンデータ書き込み
    //
    UART_printStr("--- Write Start ---\r\n");

    u16Pattern = 0;

    for(i = 0; i < NUM_SECTORS; i++)
    {
        u32Index = sectorTable[i].startAddr;

        UART_printStr("Writing: ");
        UART_printHex(sectorTable[i].startAddr);
        UART_printStr("\r\n");

        // 1セクター分（0x400 words）をWRITE_CHUNK_SIZEずつ書き込む
        for(j = 0; j < sectorTable[i].size; j += WRITE_CHUNK_SIZE, u32Index += WRITE_CHUNK_SIZE)
        {
            uint32_t k;
            for(k = 0; k < WRITE_CHUNK_SIZE; k++)
            {
                writeBuf[k] = (uint16_t)(0xA500U | (u16Pattern & 0xFFU));
                u16Pattern++;
            }

            Flash_ClearAndUnlock();

            status = Fapi_issueProgrammingCommand(
                         (uint32 *)u32Index,
                         writeBuf,
                         WRITE_CHUNK_SIZE,
                         0, 0,
                         Fapi_AutoEccGeneration);

            while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
            flashStatus = Fapi_getFsmStatus();

            if(flashStatus != 3)
            {
                UART_printStr("Write FAIL: ");
                UART_printHex(u32Index);
                UART_printStr("\r\n");
                return FLASH_TEST_PROGRAM_FAIL;
            }
        }
    }

    UART_printStr("--- Write OK ---\r\n");

    //
    // 3. 全セクターベリファイ
    //
    UART_printStr("--- Verify Start ---\r\n");

    u16Pattern = 0;

    for(i = 0; i < NUM_SECTORS; i++)
    {
        uint16_t *pFlash = (uint16_t *)sectorTable[i].startAddr;

        for(j = 0; j < sectorTable[i].size; j++)
        {
            uint16_t expected = (uint16_t)(0xA500U | (u16Pattern & 0xFFU));
            if(pFlash[j] != expected)
            {
                UART_printStr("Verify FAIL at: ");
                UART_printHex(sectorTable[i].startAddr + j);
                UART_printStr(" expected: ");
                UART_printHex(expected);
                UART_printStr(" got: ");
                UART_printHex(pFlash[j]);
                UART_printStr("\r\n");
                return FLASH_TEST_VERIFY_FAIL;
            }
            u16Pattern++;
        }
    }

    UART_printStr("--- Verify OK ---\r\n");

    return FLASH_TEST_OK;
}
