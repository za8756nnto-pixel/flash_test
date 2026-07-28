//#############################################################################
// FILE:   i2c_slave_driver.h
//
// TI C2000 (F280015x) I2C Target(スレーブ)受信ドライバ
//
// 対応フレーム:
//   Format A: [TargetAddr(0x50)+W][CMD][LEN][DATA x LEN][CRC8][STOP]
//   Format B: [TargetAddr(0x50)+W][CMD][RESTART][TargetAddr(0x50)+W][DATA...][CRC8][STOP]
//
// 受信はI2Cの「基本割込み」(ADDR_TARGET/RX_DATA_RDY/STOP_CONDITION/NO_ACK/ARB_LOST)
// で行う。FIFOは使用しない(1バイトごとにRX_DATA_RDY割込みが発生する構成)。
// ISR内ではリングバッファへのpush()のみを行う(重い処理はしない)。
// 実際のパケット処理はメインループ側でpopPacket()をポーリングして行う。
//
// 注記: TIのC2000Ware(Inclusive Terminology適用後)では、旧 "SLAVE" 系の
//       シンボルは "TARGET" に置き換えられている
//       (例: I2C_INT_ADDR_SLAVE -> I2C_INT_ADDR_TARGET,
//            I2C_STS_SLAVE_DIR  -> I2C_STS_TARGET_DIR)。
//       本ドライバは実機のi2cLib_FIFO_controller_target_interrupt.c/
//       i2c_ex5_controller_target_interrupt.c で確認済みの名称を使用している。
//
// タイムアウト:
//   tick1ms() を 1ms周期の別タイマ割込み(CPU Timer0など)から呼び出すことで、
//   フレーム受信中にSTOPが来ないまま規定時間(I2C_TIMEOUT_MS)経過した場合、
//   受信中のフレームを破棄してステートマシンをリセットする。
//#############################################################################
#ifndef I2C_SLAVE_DRIVER_H
#define I2C_SLAVE_DRIVER_H

extern "C"
{
#include "driverlib.h"
#include "device.h"
}

#include "i2c_ring_buffer.h"

//--- ユーザ設定値 ---------------------------------------------------------
#define I2C_SLAVE_ADDRESS      0x50U   // 自局スレーブアドレス
#define I2C_TIMEOUT_MS         50U     // フレーム受信タイムアウト[ms]

// 実際に使用するI2Cモジュールのベースアドレスと割込み番号は
// device.h / device_support の定義に合わせて変更すること
#ifndef I2C_SLAVE_BASE
#define I2C_SLAVE_BASE          I2CA_BASE
#endif
#ifndef I2C_SLAVE_INT
#define I2C_SLAVE_INT           INT_I2CA
#endif

class I2cSlaveDriver
{
public:
    static I2cSlaveDriver &instance()
    {
        static I2cSlaveDriver inst;
        return inst;
    }

    // I2Cモジュールの初期化(GPIOマルチプレクスは別途Device_init側で実施)
    void init(uint32_t base = I2C_SLAVE_BASE);

    // I2C割込みハンドラから呼び出す本体処理
    void isr();

    // 1msタイマ割込みから呼び出すタイムアウト監視処理
    void tick1ms();

    // ポーリング側: リングバッファから1パケット取り出す
    bool popPacket(I2cPacket &pkt) { return ring_.pop(pkt); }

    uint16_t pendingPackets() const { return ring_.available(); }
    uint32_t overrunCount() const { return ring_.overrunCount(); }

    // 統計情報(デバッグ用)
    uint32_t crcErrorCount()    const { return crcErrorCount_; }
    uint32_t lenMismatchCount() const { return lenMismatchCount_; }
    uint32_t timeoutCount()     const { return timeoutCount_; }

private:
    I2cSlaveDriver();
    I2cSlaveDriver(const I2cSlaveDriver &) = delete;
    I2cSlaveDriver &operator=(const I2cSlaveDriver &) = delete;

    // 受信ステートマシンの状態
    enum class State : uint8_t
    {
        IDLE = 0,           // 待機中(次のSTARTを待つ)
        WAIT_CMD,           // CMDバイト待ち
        WAIT_LEN_OR_RESTART,// CMDの次: LENバイト or RESTARTのどちらか
        WAIT_DATA_A,        // Format A: LEN分のデータ受信中
        WAIT_DATA_B,        // Format B: RESTART後のデータ受信中(長さ未定)
        WAIT_STOP_ONLY      // データ受信完了、STOPだけを待つ(A形式で規定数受信後)
    };

    void resetState();
    void handleAddrTarget();
    void handleRxDataReady();
    void handleStopCondition();
    void handleNack();
    void handleArbLost();

    void finalizeAndPush(bool completedNormally);
    static uint8_t crc8(const uint8_t *data, uint16_t len);

    uint32_t base_;

    volatile State state_;
    I2cPacket      current_;
    volatile uint16_t rxIndex_;      // current_.data[]への書込みインデックス
    volatile uint16_t expectedLen_;  // Format A用: LENで指定された期待データ数(CRC込み)

    // タイムアウト管理
    volatile bool     frameActive_;  // フレーム受信中かどうか
    volatile uint32_t elapsedMs_;    // フレーム開始/最終バイトからの経過時間

    // 統計情報
    uint32_t crcErrorCount_;
    uint32_t lenMismatchCount_;
    uint32_t timeoutCount_;

    I2cRingBuffer ring_;
};

// ISR(C形式)からアクセスするためのグローバル参照
extern I2cSlaveDriver &g_i2cSlave;

//--- 割込みハンドラのプロトタイプ(interrupt.hへ登録する関数) --------------
extern "C" __interrupt void i2cSlaveBasicISR(void);   // I2Cの基本割込み(ADDR_TARGET/RX_DATA_RDY/STOP等)
extern "C" void i2cSlaveTimeoutTick(void);             // 1msタイマ側から呼ぶラッパ

#endif // I2C_SLAVE_DRIVER_H
