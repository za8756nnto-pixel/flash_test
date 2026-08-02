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
#define I2C_CMD_STATUS_QUERY 0x20U           // ステータス問い合わせコマンド
                                              // [CMD]書き込み後、STOPを送らずRESTART+READで
                                              // ステータス応答フレームを取得する
#define I2C_DATA_FULL        0x00U           // 256バイト（フルデータ）
#define I2C_RX_FULL_SIZE     256U            // 1回の最大受信サイズ（バイト）
#define I2C_RX_WORDS         (I2C_RX_FULL_SIZE / 2U)  // 128words

/*--------------------------------------------------------------
 * ステータス応答（CMD=I2C_CMD_STATUS_QUERY のREAD応答）
 *
 * 「先入れ方式」：
 *   状態が変化した瞬間（アプリ起動直後／書き込み待ち／書き込み中／
 *   書き込み完了／失敗）に I2C_SetAppStatus() を呼び、
 *   [STATUS][CRC8(STATUS)] の2byteフレームを事前に組み立てて
 *   バッファに保持しておく。
 *   実際にマスターがRESTART+READしてきた瞬間（AAS割り込み）には
 *   その場で計算せず、組み立て済みバッファをTX FIFOへ
 *   コピーするだけにする。
 *--------------------------------------------------------------*/
typedef enum {
    I2C_APP_STATUS_APP_START  = 0x01U,   // アプリ起動直後
    I2C_APP_STATUS_IDLE       = 0x02U,   // コマンド／書き込み待ち
    I2C_APP_STATUS_WRITING    = 0x03U,   // Flash書き込み中
    I2C_APP_STATUS_WRITE_DONE = 0x04U,   // 書き込み完了
    I2C_APP_STATUS_WRITE_FAIL = 0x05U    // 書き込み失敗
} I2cAppStatus;

#define I2C_STATUS_FRAME_LEN  2U   // [0]=STATUS, [1]=CRC-8(STATUS)

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

// アプリのステータスをセットする（応答フレームを先入れで再生成する）。
// 状態遷移の瞬間（起動直後／待機／書き込み中／完了／失敗）に呼び出す。
void            I2C_SetAppStatus(I2cAppStatus status);

#if IRC_CODE
const uint16_t* I2C_GetRxBuffer(void);

// 割り込みハンドラ（PIEベクタテーブルに登録するため公開）
__interrupt void I2C_RxISR(void);    // RX FIFO割り込み（1byte/回の受信）
__interrupt void I2C_AddrISR(void);  // アドレス指定検出(AAS)割り込み
                                      // ※I2Cベーシック割り込み。RX FIFO割り込みとは
                                      //   別のPIEベクタ(INT_I2CA)に登録する。
                                      //   実際のベクタ名はご使用のdriverlib/
                                      //   デバイスのinterrupt.hをご確認ください。
#endif

#endif /* I2C_SLAVE_H */
