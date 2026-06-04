/*
 * flash_ctrl.c
 *
 * Flash消去・書き込み制御
 */

#include "flash_ctrl.h"
#include "uart_print.h"

//
// 消去対象セクターテーブル
// 【変更方法】
// 消去範囲を変更する場合は以下のテーブルを修正してください。
// 各エントリは { 先頭アドレス, サイズ(16bit words) } です。
//
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

// 内部書き込みバッファ（8words固定）
static uint16_t s_writeBuf[FLASH_WRITE_CHUNK_WORDS];

//
// 内部関数：FSMクリア＆保護解除
//
#pragma CODE_SECTION(Flash_ClearAndUnlock, ".TI.ramfunc")
static Fapi_StatusType Flash_ClearAndUnlock(void)
{
    Fapi_FlashStatusType flashStatus;
    Fapi_StatusType      status = Fapi_Status_Success;

    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

    flashStatus = Fapi_getFsmStatus();
    if(flashStatus != 0)
    {
        status = Fapi_issueAsyncCommand(Fapi_ClearStatus);
        while(Fapi_getFsmStatus() != 0){}
    }

    // 全セクター保護解除
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTA, 0x00000000U);
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTB, 0x00000000U);

    return status;
}

//
// Flash_CtrlInit
// Flash API初期化（main.cから一度だけ呼ぶ）
//
void Flash_CtrlInit(void)
{
    Flash_initModule(FLASH0CTRL_BASE, FLASH0ECC_BASE, DEVICE_FLASH_WAITSTATES);
    Fapi_initializeAPI((Fapi_FmcRegistersType *)CPU0_REGISTER_ADDRESS, 120U);
    Fapi_setActiveFlashBank(Fapi_FlashBank0);

    // 初期FSMクリア
    Fapi_issueAsyncCommand(Fapi_ClearStatus);
    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
}

//
// Flash_EraseRange
// 指定範囲のセクターを消去
//
#pragma CODE_SECTION(Flash_EraseRange, ".TI.ramfunc")
FlashCtrlResult Flash_EraseRange(uint32_t startAddr, uint32_t endAddr)
{
    uint32_t i;
    Fapi_StatusType          status;
    Fapi_FlashStatusType     flashStatus;
    Fapi_FlashStatusWordType flashStatusWord;

    // アドレス範囲チェック
    if(startAddr < FLASH_ERASE_START_ADDR || endAddr > FLASH_ERASE_END_ADDR)
    {
        return FLASH_CTRL_INVALID_ADDR;
    }

    for(i = 0; i < NUM_SECTORS; i++)
    {
        // 指定範囲外のセクターはスキップ
        if(sectorTable[i].startAddr < startAddr ||
           sectorTable[i].startAddr > endAddr)
        {
            continue;
        }

        Flash_ClearAndUnlock();

        status = Fapi_issueAsyncCommandWithAddress(
                     Fapi_EraseSector,
                     (uint32 *)sectorTable[i].startAddr);

        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
        flashStatus = Fapi_getFsmStatus();

        if(flashStatus != 3)
        {
            return FLASH_CTRL_ERASE_FAIL;
        }

        // 消去ベリファイ
        status = Fapi_doBlankCheck(
                     (uint32 *)sectorTable[i].startAddr,
                     sectorTable[i].sizeWords / 2,
                     &flashStatusWord);

        if(status != Fapi_Status_Success)
        {
            return FLASH_CTRL_BLANKCHECK_FAIL;
        }
    }

    return FLASH_CTRL_OK;
}

//
// Flash_WriteData
// 指定アドレスにデータを書き込む
// pData:     書き込みデータ（16bit words配列）
// sizeWords: 書き込みサイズ（16bit words単位）
//            I2C受信の256バイトの場合は128を指定
//
#pragma CODE_SECTION(Flash_WriteData, ".TI.ramfunc")
FlashCtrlResult Flash_WriteData(uint32_t destAddr, uint16_t *pData, uint32_t sizeWords)
{
    uint32_t i;
    uint32_t u32Index;
    Fapi_StatusType      status;
    Fapi_FlashStatusType flashStatus;

    // アドレス範囲チェック
    if(destAddr < FLASH_ERASE_START_ADDR ||
       destAddr + sizeWords > FLASH_ERASE_END_ADDR)
    {
        return FLASH_CTRL_INVALID_ADDR;
    }

    u32Index = destAddr;

    // FLASH_WRITE_CHUNK_WORDS（8words）単位に分割して書き込む
    for(i = 0; i < sizeWords; i += FLASH_WRITE_CHUNK_WORDS,
                               u32Index += FLASH_WRITE_CHUNK_WORDS)
    {
        uint32_t k;

        // 書き込みバッファにコピー
        for(k = 0; k < FLASH_WRITE_CHUNK_WORDS; k++)
        {
            s_writeBuf[k] = pData[i + k];
        }

        Flash_ClearAndUnlock();

        status = Fapi_issueProgrammingCommand(
                     (uint32 *)u32Index,
                     s_writeBuf,
                     FLASH_WRITE_CHUNK_WORDS,
                     0, 0,
                     Fapi_AutoEccGeneration);

        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
        flashStatus = Fapi_getFsmStatus();

        if(flashStatus != 3)
        {
            return FLASH_CTRL_WRITE_FAIL;
        }
    }

    return FLASH_CTRL_OK;
}
