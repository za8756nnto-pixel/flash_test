/**************************************************
 * @file    flash_ctrl.c
 * @brief   Flash消去・書き込み制御モジュール
 *
 * @details
 * F2800157デバイスのFlashメモリに対して、
 * 消去・書き込み・ベリファイ操作を実装するモジュール。
 * すべてのFlash操作関数は .TI.ramfunc セクションに配置され、
 * RAM上で実行される。
 *
 * 【修正履歴】
 * 2026/06/11 Fapi_issueProgrammingCommand の第3引数を修正
 *            誤: FLASH_WRITE_WORDS (16bit words数)
 *            正: FLASH_WRITE_WORDS / 2U (32bit words数)
 *            → ECCが誤位置に書かれてデータ領域が破損する問題を修正
 *
 * @date    2026/06/04
 * @author  user
 **************************************************/

//---------------------------
// インクルード
//---------------------------
#include "flash_ctrl.h"
#include "uart_print.h"

//-------------------------------------------
// 消去対象セクターテーブル
// 【変更方法】
// 消去範囲を変更する場合は以下のテーブルを修正してください。
// 各エントリは { 先頭アドレス, サイズ(16bit words) } です。
//-------------------------------------------
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
static uint16_t s_writeBuf[FLASH_WRITE_WORDS];


/***************************************************************
 * @brief   FSMステータスクリアおよび書き込み保護解除（内部関数）
 *
 * @details
 * Flash操作前に毎回呼び出す内部共通処理。
 * 以下の順で処理を実行する：
 *   1. FSMが準備完了状態になるまで待機
 *   2. FSMステータスを確認し、エラーがあればクリア
 *   3. CMDWEPROTA/B に全セクター許可値を設定
 *
 * @return  Fapi_StatusType  Fapi_ClearStatus の戻り値
 * @note    本関数は .TI.ramfunc セクションに配置されRAM上で実行される
 ***************************************************************/
#pragma CODE_SECTION(Flash_ClearAndUnlock, ".TI.ramfunc")
static Fapi_StatusType Flash_ClearAndUnlock(void)
{
    Fapi_FlashStatusType flashStatus;
    Fapi_StatusType      status = Fapi_Status_Success;

    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

    flashStatus = Fapi_getFsmStatus();
    if(flashStatus != FLASH_FSM_STATUS_IDLE)
    {
        status = Fapi_issueAsyncCommand(Fapi_ClearStatus);
        while(Fapi_getFsmStatus() != FLASH_FSM_STATUS_IDLE){}
    }

    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTA, 0x00000000U);
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTB, 0x00000000U);

    return status;
}

/***************************************************************
 * @brief   Flash API初期化（main.c から一度だけ呼ぶ）
 ***************************************************************/
void Flash_CtrlInit(void)
{
    Flash_initModule(FLASH0CTRL_BASE, FLASH0ECC_BASE, DEVICE_FLASH_WAITSTATES);

    Fapi_initializeAPI(FlashTech_CPU0_BASE_ADDRESS, DEVICE_SYSCLK_FREQ / 1000000U);

    Fapi_setActiveFlashBank(Fapi_FlashBank0);

    Fapi_issueAsyncCommand(Fapi_ClearStatus);

    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
}

/***************************************************************
 * @brief   指定範囲のセクターを消去
 ***************************************************************/
#pragma CODE_SECTION(Flash_EraseRange, ".TI.ramfunc")
FlashCtrlResult Flash_EraseRange(uint32_t startAddr, uint32_t endAddr)
{
    uint32_t                 i;
    Fapi_StatusType          status;
    Fapi_FlashStatusType     flashStatus;
    Fapi_FlashStatusWordType flashStatusWord;

    if((startAddr < FLASH_ERASE_START_ADDR) ||
       (endAddr   > FLASH_ERASE_END_ADDR))
    {
        return FLASH_CTRL_INVALID_ADDR;
    }

    for(i = 0U; i < NUM_SECTORS; i++)
    {
        if((sectorTable[i].startAddr < startAddr) ||
           (sectorTable[i].startAddr > endAddr))
        {
            continue;
        }

        Flash_ClearAndUnlock();

        status = Fapi_issueAsyncCommandWithAddress(
                     Fapi_EraseSector,
                     (uint32 *)sectorTable[i].startAddr);

        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

        flashStatus = Fapi_getFsmStatus();

        if(flashStatus != FLASH_FSM_STATUS_DONE)
        {
            return FLASH_CTRL_ERASE_FAIL;
        }

        // BlankCheck: 第2引数は u32length = 16bit sizeWords / 2
        status = Fapi_doBlankCheck(
                     (uint32 *)sectorTable[i].startAddr,
                     sectorTable[i].sizeWords / 2U,
                     &flashStatusWord);

        if(status != Fapi_Status_Success)
        {
            return FLASH_CTRL_BLANKCHECK_FAIL;
        }
    }

    return FLASH_CTRL_OK;
}

/***************************************************************
 * @brief   指定アドレスにデータを書き込む
 *
 * @param   destAddr  書き込み先アドレス（16bit word アドレス）
 * @param   pData     書き込みデータ（16bit words配列）
 * @param   sizeWords 書き込みサイズ（16bit words単位）
 *
 * @details
 * 【Fapi_issueProgrammingCommand 第3引数について】
 *
 * Fapi APIのプロトタイプ:
 *   Fapi_issueProgrammingCommand(
 *       uint32  *pu32StartAddress,
 *       uint16  *pu16DataBuffer,
 *       uint16   u16DataBufferSizeInWords,  ← 16bit words数
 *       uint16  *pu16EccBuffer,
 *       uint16   u16EccBufferSizeInBytes,
 *       Fapi_FlashProgrammingCommandsType  oFlashProgrammingCommand)
 *
 * C2000 TRM および Fapi ユーザーガイドの記載:
 *   u16DataBufferSizeInWords は「16bit words数」であるが、
 *   内部実装では 32bit (2×16bit) 単位でFSMに送るため、
 *   実際に渡す値は「32bit words数 = 16bit words数 / 2」にする必要がある。
 *
 *   FLASH_WRITE_WORDS = 8（16bit words）の場合:
 *     誤: 8  → FSMが16wordsぶん書き込もうとし、アドレスが2倍ずれる
 *              → ECCが誤位置に書かれ、データ領域に 0x8401 等が混入する
 *     正: 4  → FSMが正しく8wordsぶん書き込む
 *
 *   参考: C2000Ware FlashAPI サンプルコード (flash_programming_example.c)
 *         Fapi_issueProgrammingCommand() の呼び出し箇所
 ***************************************************************/
#pragma CODE_SECTION(Flash_WriteData, ".TI.ramfunc")
FlashCtrlResult Flash_WriteData(uint32_t destAddr, uint16_t *pData, uint32_t sizeWords)
{
    uint32_t             i;           // データインデックス（16bit words単位）
    uint32_t             k;           // バッファコピー用ループカウンタ
    uint32_t             u32Index;    // 現在の書き込みアドレス
    Fapi_StatusType      status;
    Fapi_FlashStatusType flashStatus;

    if((destAddr             < FLASH_ERASE_START_ADDR) ||
       (destAddr + sizeWords > FLASH_ERASE_END_ADDR))
    {
        return FLASH_CTRL_INVALID_ADDR;
    }

    u32Index = destAddr;

    for(i = 0U; i < sizeWords; i        += FLASH_WRITE_WORDS,
                                u32Index += FLASH_WRITE_WORDS)
    {
        // 内部バッファに今回書き込む 8words 分のデータをコピー
        for(k = 0U; k < FLASH_WRITE_WORDS; k++)
        {
            s_writeBuf[k] = pData[i + k];
        }

        Flash_ClearAndUnlock();

        // -------------------------------------------------------
        // 【修正箇所】第3引数: FLASH_WRITE_WORDS / 2U
        //
        // Fapi_issueProgrammingCommand の内部は 32bit 単位で動作する。
        // 16bit words 数をそのまま渡すと書き込みサイズが2倍になり、
        // ECC計算位置がずれてデータ領域に誤った値（例: 0x8401）が
        // 書き込まれる。
        // 正しくは「16bit words数 ÷ 2 = 32bit words数」を渡す。
        // -------------------------------------------------------
        status = Fapi_issueProgrammingCommand(
                     (uint32 *)u32Index,
                     s_writeBuf,
                     FLASH_WRITE_WORDS / 2U,   // ← 修正: 8/2 = 4 (32bit words)
                     0U,
                     0U,
                     Fapi_AutoEccGeneration);

        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

        flashStatus = Fapi_getFsmStatus();

        if(flashStatus != FLASH_FSM_STATUS_DONE)
        {
            return FLASH_CTRL_WRITE_FAIL;
        }
    }

    return FLASH_CTRL_OK;
}

/***************************************************************
 * @brief   FW書き込みフラグ読み出し
 *
 * @retval  true  : FW書き込み要求あり（値 == 0x0001）
 * @retval  false : 要求なし
 ***************************************************************/
bool Flash_IsFwUpdateFlg(void)
{
    volatile uint16_t *pFlag = (volatile uint16_t *)FLASH_NVM_FLAG_ADDR;
    return (*pFlag == FLASH_NVM_FLAG_UPDATE);
}

/***************************************************************
 * @brief   FW書き込みフラグクリア
 *
 * @details
 * NVMセクターを消去後、フラグアドレスに 0x0000 を書き込む。
 *
 * @retval  FLASH_CTRL_OK          正常完了
 * @retval  FLASH_CTRL_ERASE_FAIL  消去失敗
 * @retval  FLASH_CTRL_WRITE_FAIL  書き込み失敗
 ***************************************************************/
#pragma CODE_SECTION(Flash_ClearFwUpdateFlag, ".TI.ramfunc")
FlashCtrlResult Flash_ClearFwUpdateFlag(void)
{
    Fapi_StatusType      status;
    Fapi_FlashStatusType flashStatus;

    uint16_t flagBuf[FLASH_WRITE_WORDS];
    uint32_t k;
    for(k = 0U; k < FLASH_WRITE_WORDS; k++)
    {
        flagBuf[k] = 0xFFFFU;
    }
    flagBuf[0] = FLASH_NVM_FLAG_DONE;

    //
    // 1. NVMセクターを消去する
    //
    Flash_ClearAndUnlock();

    status = Fapi_issueAsyncCommandWithAddress(
                 Fapi_EraseSector,
                 (uint32 *)FLASH_NVM_SECTOR_ADDR);

    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

    flashStatus = Fapi_getFsmStatus();
    if(flashStatus != FLASH_FSM_STATUS_DONE)
    {
        return FLASH_CTRL_ERASE_FAIL;
    }

    //
    // 2. フラグアドレスに 0x0000 を書き込む
    //    【修正箇所】第3引数: FLASH_WRITE_WORDS / 2U
    //
    Flash_ClearAndUnlock();

    status = Fapi_issueProgrammingCommand(
                 (uint32 *)FLASH_NVM_FLAG_ADDR,
                 flagBuf,
                 FLASH_WRITE_WORDS / 2U,       // ← 修正: 8/2 = 4 (32bit words)
                 0U,
                 0U,
                 Fapi_AutoEccGeneration);

    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

    flashStatus = Fapi_getFsmStatus();
    if(flashStatus != FLASH_FSM_STATUS_DONE)
    {
        return FLASH_CTRL_WRITE_FAIL;
    }

    return FLASH_CTRL_OK;
}
