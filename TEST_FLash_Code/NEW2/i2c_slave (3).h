/*
 * i2c_slave.h
 *
 * I2Cスレーブ受信・Flash書き込み制御
 *
 * 【変更方法】
 * I2C転送速度を変更する場合は I2C_BAUDRATE を修正してください。
 * ピン設定を変更する場合は I2C_SDA_PIN / I2C_SCL_PIN を修正してください。
 * 対応コマンドを追加する場合は I2C_CMD_xxx の定義と
 * i2c_slave.c の I2C_RxISR 内 switch(s_rxState==WAIT_CMD) の
 * 分岐、および I2C_FwUpdate 内の dispatch 処理を修正してください。
 */

#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include "driverlib.h"
#include "device.h"
#include "flash_ctrl.h"
#include "pin_map.h"

/*--------------------------------------------------------------
 * I2C設定
 * 【変更する場合はここを修正】
 *--------------------------------------------------------------*/
#define I2C_SLAVE_ADDRESS    0x50U           // スレーブアドレス
#define I2C_BAUDRATE         100000U         // 転送速度（100kHz）
#define I2C_SDA_PIN          28U             // SDA GPIO番号
#define I2C_SCL_PIN          27U             // SCL GPIO番号
#define I2C_SDA_CFG          GPIO_0_I2CA_SDA
#define I2C_SCL_CFG          GPIO_1_I2CA_SCL

/*--------------------------------------------------------------
 * プロトコル定義
 *--------------------------------------------------------------*/
#define I2C_CMD_FW_WRITE     0x17U           // FW書き込みコマンド
#define I2C_CMD_STATUS_REQ   0x20U           // ステータス要求コマンド
                                              // [CMD]のみ送信後、STOPを打たずに
                                              // RESTART+READでスレーブの状態を読む
#define I2C_DATA_FULL        0x00U           // 256バイト（フルデータ）
#define I2C_RX_FULL_SIZE     256U            // 1回の最大受信サイズ（バイト）
#define I2C_RX_WORDS         (I2C_RX_FULL_SIZE / 2U)  // 128words

/*--------------------------------------------------------------
 * アプリ状態（I2C_CMD_STATUS_REQ への応答値）
 *   マスタはCMD=0x20送信後、STOPなしのRESTART+READで
 *   この値を1バイト読み出す。
 *--------------------------------------------------------------*/
typedef enum {
    I2C_APP_STATE_WAITING_FOR_WRITE = 0x00U,  // 書き込み待ち（アイドル）
    I2C_APP_STATE_WRITING           = 0x01U,  // 書き込み中
    I2C_APP_STATE_WRITE_COMPLETE    = 0x02U,  // 書き込み完了
    I2C_APP_STATE_APP_RUNNING       = 0x03U   // アプリ起動中
} I2cAppState;

/*--------------------------------------------------------------
 * ISR受信ステートマシン
 *   WAIT_CMD  : コマンドバイト待ち
 *   WAIT_LEN  : 長さバイト待ち
 *   RX_DATA   : データ本体受信中
 *--------------------------------------------------------------*/
typedef enum {
    I2C_RXSTATE_WAIT_CMD = 0,
    I2C_RXSTATE_WAIT_LEN,
    I2C_RXSTATE_RX_DATA
} I2cRxState;

//-------------------------------------------------------------
//	コンパイルスイッチ
//-------------------------------------------------------------
#define IRC_CODE	 1			//　割り込み(1)　と　ポーリング(0)の切り替え

/*--------------------------------------------------------------
 * 戻り値
 *--------------------------------------------------------------*/
typedef enum {
    I2C_SLAVE_OK = 0,
    I2C_SLAVE_FLASH_ERASE_FAIL,
    I2C_SLAVE_FLASH_WRITE_FAIL,
    I2C_SLAVE_INVALID_CMD,
    I2C_SLAVE_TIMEOUT,
} I2cSlaveResult;

/*--------------------------------------------------------------
 * 公開関数
 *--------------------------------------------------------------*/
void            I2C_SlaveInit(void);
I2cSlaveResult  I2C_FwUpdate(void);

// アプリ状態を更新する（Flash書き込み処理やmain()から呼ぶ）
// I2C_CMD_STATUS_REQ 応答に反映される
void            I2C_SetAppState(I2cAppState state);

#if IRC_CODE
const uint16_t* I2C_GetRxBuffer(void);

// 割り込みハンドラ（PIEベクタテーブルに登録するため公開）
__interrupt void I2C_RxISR(void);

// STOPコンディション検出ハンドラ（basic割り込みベクタ INT_I2CA）
// 想定外タイミングでのSTOPを検出し、受信ステートマシンを
// WAIT_CMDへ強制リセットする（次のCMDを確実に取りこぼさないため）
__interrupt void I2C_EventISR(void);
#endif

#endif /* I2C_SLAVE_H */
