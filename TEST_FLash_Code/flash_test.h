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

// 検証対象セクター
#define TEST_FLASH_START_ADDR    Bzero_Sector16_start
#define TEST_FLASH_END_ADDR      0x00084400UL
#define TEST_DATA_SIZE           128U

typedef enum {
    FLASH_TEST_OK = 0,
    FLASH_TEST_ERASE_FAIL,
    FLASH_TEST_PROGRAM_FAIL,
    FLASH_TEST_VERIFY_FAIL
} FlashTestResult;

extern FlashTestResult Flash_RunTest(void);

#endif
