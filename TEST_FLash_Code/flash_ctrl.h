/*
 * flash_ctrl.h
 *
 * Flash消去・書き込み制御
 *
 * 【消去範囲の変更方法】
 * FLASH_ERASE_START_ADDR と FLASH_ERASE_END_ADDR を変更してください。
 * セクター境界に合わせる必要があります。
 * 例：
 *   Sector7  先頭: 0x081C00
 *   Sector28 終端: 0x0873FF
 */

#ifndef FLASH_CTRL_H
#define FLASH_CTRL_H

#include "driverlib.h"
#include "device.h"
#include "FlashAPI/FlashTech_F280015x_C28x.h"
#include "flash_programming_f280015x.h"

//
// 消去対象範囲（変更する場合はここを修正）
//
#define FLASH_ERASE_START_ADDR    0x081C00U  // Sector7  先頭
#define FLASH_ERASE_END_ADDR      0x0873FFU  // Sector28 終端

//
// 書き込みチャンクサイズ（変更不可：Flash API上限）
//
#define FLASH_WRITE_CHUNK_WORDS   8U         // 8words = 16バイト固定

//
// 書き込みデータサイズ（I2C受信単位）
//
#define FLASH_WRITE_DATA_WORDS    128U       // 128words = 256バイト

//
// セクター情報
//
typedef struct {
    uint32_t startAddr;
    uint32_t sizeWords;  // 16bit words単位
} FlashSectorInfo;

//
// 戻り値
//
typedef enum {
    FLASH_CTRL_OK = 0,
    FLASH_CTRL_ERASE_FAIL,
    FLASH_CTRL_BLANKCHECK_FAIL,
    FLASH_CTRL_WRITE_FAIL,
    FLASH_CTRL_VERIFY_FAIL,
    FLASH_CTRL_INVALID_ADDR,
} FlashCtrlResult;

//
// 公開関数
//

// Flash API初期化（main.cから一度だけ呼ぶ）
void Flash_CtrlInit(void);

// 指定範囲のセクターを消去
// startAddr: 消去開始アドレス（FLASH_ERASE_START_ADDR以上）
// endAddr:   消去終了アドレス（FLASH_ERASE_END_ADDR以下）
FlashCtrlResult Flash_EraseRange(uint32_t startAddr, uint32_t endAddr);

// 指定アドレスにデータを書き込む
// destAddr:  書き込み先アドレス
// pData:     書き込みデータ（128words = 256バイト）
// sizeWords: 書き込みサイズ（16bit words単位）
FlashCtrlResult Flash_WriteData(uint32_t destAddr, uint16_t *pData, uint32_t sizeWords);

#endif /* FLASH_CTRL_H */
