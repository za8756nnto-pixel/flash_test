/*
 * i2c_slave.h
 *
 * I2Cスレーブ受信・Flash書き込み制御
 *
 * 【変更方法】
 * I2C転送速度を変更する場合は I2C_BAUDRATE を修正してください。
 * ピン設定を変更する場合は I2C_SDA_PIN / I2C_SCL_PIN を修正してください。
 */

#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include "driverlib.h"
#include "device.h"
#include "flash_ctrl.h"
#include "pin_map.h"
//
// I2C設定
// 【変更する場合はここを修正】
//
#define I2C_SLAVE_ADDRESS    0x50U           // スレーブアドレス
#define I2C_BAUDRATE         100000U         // 転送速度（100kHz）
#define I2C_SDA_PIN          28U             // SDA GPIO番号
#define I2C_SCL_PIN          27U             // SCL GPIO番号
#define I2C_SDA_CFG          GPIO_0_I2CA_SDA
#define I2C_SCL_CFG          GPIO_1_I2CA_SCL

//
// プロトコル定義
//
#define I2C_CMD_FW_WRITE     0x17U           // FW書き込みコマンド
#define I2C_DATA_FULL        0x00U           // 256バイト（フルデータ）
#define I2C_RX_FULL_SIZE     256U            // 1回の最大受信サイズ（バイト）
#define I2C_RX_WORDS         (I2C_RX_FULL_SIZE / 2U)  // 128words

//
// 戻り値
//
typedef enum {
    I2C_SLAVE_OK = 0,
    I2C_SLAVE_FLASH_ERASE_FAIL,
    I2C_SLAVE_FLASH_WRITE_FAIL,
    I2C_SLAVE_INVALID_CMD,
} I2cSlaveResult;

//
// 公開関数
//
void I2C_SlaveInit(void);
I2cSlaveResult I2C_FwUpdate(void);

#endif /* I2C_SLAVE_H */
