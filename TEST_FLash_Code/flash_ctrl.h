/***************************************************************
 * @file    flash_ctrl.h
 * @brief   Flash消去・書き込み制御モジュール ヘッダファイル
 *
 * @details
 * F2800157デバイスのFlashメモリに対して、
 * 消去・書き込み・ベリファイ操作を提供するモジュール。
 *
 * 【消去範囲の変更方法】
 * FLASH_ERASE_START_ADDR と FLASH_ERASE_END_ADDR を変更してください。
 * セクター境界に合わせる必要があります。
 * 例：
 *   Sector7  先頭: 0x081C00
 *   Sector28 終端: 0x0873FF
 *
 * @note
 * - 本モジュールの関数はすべてRAM上で実行されます（.TI.ramfunc）
 * - Flash_CtrlInit() は main() から一度だけ呼び出してください
 * - NVM領域（0x087400〜）は本モジュールでは操作しません
 *
 * @date    2026/06/04
 * @author  user
 ***************************************************************/
#ifndef FLASH_CTRL_H
#define FLASH_CTRL_H

//---------------------------------
// インクルード
//---------------------------------
#include "driverlib.h"
#include "device.h"
#include "FlashAPI/FlashTech_F280015x_C28x.h"
#include "flash_programming_f280015x.h"


//----------------------------------
// 消去対象範囲（変更する場合はここを修正）
//----------------------------------
#define FLASH_ERASE_START_ADDR    0x081C00U  // Sector7  先頭
#define FLASH_ERASE_END_ADDR      0x0873FFU  // Sector28 終端


//-----------------------------------
// 書き込みサイズ（変更不可：Flash API上限）
//
// FLASH_WRITE_CHUNK_WORDS: 1回の書き込みで書き込めるデータの量（バイト単位）。
// Flash APIの制限により、この値は固定されています。
// 通常は8バイト（16bit words * 2）で、1回の書き込みで8バイトまで書き込めます。
//-----------------------------------
#define FLASH_WRITE_WORDS   8U         // 8words = 16バイト固定

//------------------------------------
// 書き込みデータサイズ（I2C受信単位）
//
// FLASH_WRITE_DATA_WORDS: 1回の書き込みで書き込むデータ（words単位）。
// I2Cで受信するデータの量（words単位）を指定します。
// この値とFLASH_WRITE_CHUNK_WORDSを組み合わせて、書き込むデータを分割します。
// 例：Flash_WriteData(destAddr, pData, FLASH_WRITE_DATA_WORDS)
// 128 words = 256バイトの場合
//-------------------------------------
#define FLASH_WRITE_DATA_WORDS    128U       // 128words = 256バイト

//FSMアイドル状態（エラーなし、操作前の初期状態）
#define FLASH_FSM_STATUS_IDLE     0U
// FSM正常完了（FSM_DONE ビット + エラーなし）
#define FLASH_FSM_STATUS_DONE     3U

//CMDWEPROTA 全セクター保護解除（Sector0〜31 全許可）
#define FLASH_WEPROTA_ALL_UNLOCK  0x00000000U
//CMDWEPROTB 全セクター保護解除（Sector32〜 全許可）
#define FLASH_WEPROTB_ALL_UNLOCK  0x00000000U

//----------------------------------
// NVM領域定義（FW書き込みフラグ）
// 【変更方法】
// フラグのアドレスを変更する場合は FLASH_NVM_FLAG_ADDR を修正してください。
//----------------------------------
#define FLASH_NVM_FLAG_ADDR     0x087400U   // FW書き込みフラグ格納アドレス
#define FLASH_NVM_FLAG_UPDATE   0x0001U     // FW書き込み要求フラグ値
#define FLASH_NVM_FLAG_DONE     0x0000U     // FW書き込み完了フラグ値
// NVMセクター（フラグクリア時の消去対象）
#define FLASH_NVM_SECTOR_ADDR   0x087400U   // NVMセクター先頭
#define FLASH_NVM_SECTOR_SIZE   0x0400U     // NVMセクターサイズ（words）

//-------------------------------------
// 構造体
//-------------------------------------

// セクター情報
// Flashのセクターに関する情報を持つ構造体
typedef struct {
    uint32_t startAddr;  // セクターの開始アドレス
    uint32_t sizeWords;  // セクターのサイズ（16bit words単位）
} FlashSectorInfo;

//--------------------------------------
// enum
//--------------------------------------
//Flash制御操作の結果
typedef enum {
    FLASH_CTRL_OK = 0,          //操作が正常に完了した
    FLASH_CTRL_ERASE_FAIL,      //消去操作が失敗した
    FLASH_CTRL_BLANKCHECK_FAIL, //Blankチェックが失敗した
    FLASH_CTRL_WRITE_FAIL,      //書き込み操作が失敗した
    FLASH_CTRL_VERIFY_FAIL,     //データの検証が失敗した
    FLASH_CTRL_INVALID_ADDR,    //無効なアドレスが指定された
} FlashCtrlResult;

//---------------------------------------
// プロトタイプ宣言
//---------------------------------------
void Flash_CtrlInit(void);  // Flash API初期化（main.cから一度だけ呼ぶ）
FlashCtrlResult Flash_EraseRange(uint32_t startAddr, uint32_t endAddr); //指定範囲のセクターを消去
FlashCtrlResult Flash_WriteData(uint32_t destAddr, uint16_t *pData, uint32_t sizeWords); // 指定アドレスにデータを書き込む
bool            Flash_IsFwUpdateFlg(void);        // フラグ読み出し
FlashCtrlResult Flash_ClearFwUpdateFlag(void);    // フラグクリア（0に書き換え）
#endif /* FLASH_CTRL_H */
