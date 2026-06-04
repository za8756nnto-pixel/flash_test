/*
 * flash_test.h
 *
 *  Created on: 2026/06/04
 *      Author: user
 */
#ifndef FLASH_TEST_H
#define FLASH_TEST_H

#include "driverlib.h"
#include "device.h"
#include "FlashAPI/FlashTech_F280015x_C28x.h"

// 検証対象範囲
#define TEST_FLASH_START_ADDR    0x081C00U  // Sector7先頭
#define TEST_FLASH_END_ADDR      0x0873FFU  // Sector28終端

// 書き込みテストデータサイズ（16bit words単位）
#define TEST_DATA_SIZE           128U

// セクター情報テーブル用
typedef struct {
    uint32_t startAddr;
    uint32_t size;        // 16bit words単位
} FlashSectorInfo;

typedef enum {
    FLASH_TEST_OK = 0,
    FLASH_TEST_ERASE_FAIL,
    FLASH_TEST_PROGRAM_FAIL,
    FLASH_TEST_VERIFY_FAIL
} FlashTestResult;

extern FlashTestResult Flash_RunTest(void);

#endif
