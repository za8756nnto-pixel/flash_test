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
#define NUM_SECTORS  (sizeof(sectorTable) / sizeof(sectorTable[0]))     //テーブル数

// 内部書き込みバッファ（8words固定）
static uint16_t s_writeBuf[FLASH_WRITE_WORDS];    // 内部書き込みバッファ（8words固定）


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
 * @param   なし
 * @return  Fapi_StatusType  Fapi_ClearStatus の戻り値
 *                           エラーなしの場合は Fapi_Status_Success
 *
 * @note    本関数は .TI.ramfunc セクションに配置されRAM上で実行される
 * @note    pragmaの制約上、プロトタイプ宣言は記載しない
 ***************************************************************/
#pragma CODE_SECTION(Flash_ClearAndUnlock, ".TI.ramfunc")
static Fapi_StatusType Flash_ClearAndUnlock(void)
{
    Fapi_FlashStatusType flashStatus;
    Fapi_StatusType      status = Fapi_Status_Success;
    // FSMが準備状態になるまで待機します。
    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
    // FSMの状態を取得します。
    flashStatus = Fapi_getFsmStatus();
    if(flashStatus != FLASH_FSM_STATUS_IDLE)
    {
        // FSMの状態が0でない場合、非同期コマンドを発行します。
        status = Fapi_issueAsyncCommand(Fapi_ClearStatus);
        // FSMの状態が0でないまで待機します。
        while(Fapi_getFsmStatus() != FLASH_FSM_STATUS_IDLE){}
    }

    // 全セクター保護解除
    // FLASH_WRAPPER_PROGRAM_BASEは、Flashメモリへの書き込みアクセスに使用されるベースアドレスです。
    // FLASH_O_CMDWEPROTAとFLASH_O_CMDWEPROTBは、Flashメモリのコマンド書き込み保護ビットです。
    // これらのビットをクリアすることで、Flashメモリの保護状態を解除します。
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTA, 0x00000000U);
    Fapi_setupBankSectorEnable(FLASH_WRAPPER_PROGRAM_BASE + FLASH_O_CMDWEPROTB, 0x00000000U);

    return status;
}

/**************************************************************
 *
 * Flashメモリの指定範囲を消去します。
 *
 *
 *
 *
 **************************************************************/
void Flash_CtrlInit(void)
{
    /* Flash Wait State を設定する（クロック周波数に応じた待機サイクル） */
    Flash_initModule(FLASH0CTRL_BASE, FLASH0ECC_BASE, DEVICE_FLASH_WAITSTATES);

    /* Fapi API を初期化する
     * 第1引数: FMCレジスタのベースアドレス
     * 第2引数: CPUクロック周波数（MHz単位） */
    Fapi_initializeAPI(FlashTech_CPU0_BASE_ADDRESS, DEVICE_SYSCLK_FREQ / 1000000U);

    /* 操作対象のFlashバンクを Bank0 に設定する */
    Fapi_setActiveFlashBank(Fapi_FlashBank0);

    /* 起動時の初期FSMステータスをクリアする */
    Fapi_issueAsyncCommand(Fapi_ClearStatus);

    /* FSMがクリア完了し準備状態になるまで待機する */
    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}
}

//
// Flash_EraseRange
// 指定範囲のセクターを消去
//
#pragma CODE_SECTION(Flash_EraseRange, ".TI.ramfunc")
FlashCtrlResult Flash_EraseRange(uint32_t startAddr, uint32_t endAddr)
{
      uint32_t                 i;               // ループカウンタ
      Fapi_StatusType          status;          // Flash API 戻り値
      Fapi_FlashStatusType     flashStatus;     // FSMステータス
      Fapi_FlashStatusWordType flashStatusWord; // BlankCheck結果

      //引数のアドレスが許容範囲内かチェックする
      if((startAddr < FLASH_ERASE_START_ADDR) ||
         (endAddr   > FLASH_ERASE_END_ADDR))
      {
          //範囲外の場合は即座にエラーを返す
          return FLASH_CTRL_INVALID_ADDR;
      }

      //sectorTable の全エントリを順番に処理する
      for(i = 0U; i < NUM_SECTORS; i++)
      {
          //指定範囲外のセクターはスキップする
          if((sectorTable[i].startAddr < startAddr) ||
             (sectorTable[i].startAddr > endAddr))
          {
              continue;
          }

          //FSMクリアおよび書き込み保護解除を実施する
          Flash_ClearAndUnlock();

          //セクター消去コマンドを発行する
          status = Fapi_issueAsyncCommandWithAddress(
                       Fapi_EraseSector,
                       (uint32 *)sectorTable[i].startAddr);

          //消去完了（FSM準備完了状態）になるまでポーリング待機する
          while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

          //消去後のFSMステータスを確認する
          flashStatus = Fapi_getFsmStatus();


          if(flashStatus != FLASH_FSM_STATUS_DONE)
          {
              //FSMステータスが正常完了（DONE）でなければ消去失敗とする
              return FLASH_CTRL_ERASE_FAIL;
          }

          //消去ベリファイ（BlankCheck）を実施する
          //第2引数はu32length = u16sizeWords / 2
          status = Fapi_doBlankCheck(
                       (uint32 *)sectorTable[i].startAddr,
                       sectorTable[i].sizeWords / 2U,
                       &flashStatusWord);


          if(status != Fapi_Status_Success)
          {
              //BlankCheck が Success でなければベリファイ失敗とする
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
    uint32_t             i;           // データインデックス（words単位）
    uint32_t             k;           // バッファコピー用ループカウンタ
    uint32_t             u32Index;    // 現在の書き込みアドレス
    Fapi_StatusType      status;      // Flash API 戻り値
    Fapi_FlashStatusType flashStatus; // FSMステータス

    //引数のアドレスおよびサイズが許容範囲内かチェックする
    if((destAddr             < FLASH_ERASE_START_ADDR) ||
       (destAddr + sizeWords > FLASH_ERASE_END_ADDR))
    {
        //範囲外の場合は即座にエラーを返す
        return FLASH_CTRL_INVALID_ADDR;
    }

    //書き込み先アドレスを初期化する
    u32Index = destAddr;

    //FLASH_WRITE_WORDS（8words）単位に分割して書き込む
    for(i = 0U; i < sizeWords; i        += FLASH_WRITE_WORDS,
                                u32Index += FLASH_WRITE_WORDS)
    {
        //内部バッファに今回書き込む8words分のデータをコピーする
        for(k = 0U; k < FLASH_WRITE_WORDS; k++)
        {
            s_writeBuf[k] = pData[i + k];
        }

        //FSMクリアおよび書き込み保護解除を実施する
        Flash_ClearAndUnlock();

        // 書き込みコマンドを発行する
        // ECC は Fapi_AutoEccGeneration で自動生成する
        status = Fapi_issueProgrammingCommand(
                     (uint32 *)u32Index,
                     s_writeBuf,
                     FLASH_WRITE_WORDS,
                     0U,
                     0U,
                     Fapi_AutoEccGeneration);

        // 書き込み完了（FSM準備完了状態）になるまでポーリング待機する
        while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady){}

        // 書き込み後のFSMステータスを確認する
        flashStatus = Fapi_getFsmStatus();

        if(flashStatus != FLASH_FSM_STATUS_DONE)
        {
            // FSMステータスが正常完了（DONE）でなければ書き込み失敗とする
            return FLASH_CTRL_WRITE_FAIL;
        }
    }

    return FLASH_CTRL_OK;
}
