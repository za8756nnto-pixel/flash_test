#ifndef PERIPHERAL_I2C_CI2CROLESWITCHER_HPP_
#define PERIPHERAL_I2C_CI2CROLESWITCHER_HPP_

#include <stdint.h>
#include <stdbool.h>
#include <common/UserDefine.hpp>

extern "C" {
#include "device.h"
#include "driverlib.h"
}

#define I2C_STS_EVENT_FLAGS ( \
    I2C_STS_ARB_LOST | \
    I2C_STS_NO_ACK | \
    I2C_STS_STOP_CONDITION)

#define I2C_DEVICE_START_DISEBLE 0

//=====================================================================
// フレームフォーマット定義
//   [コマンド(1)] [データ長(1)] [データ(データ長)] [CRC(1)]
// ※ CRCは仮に1バイト(CRC8)としています。実際のプロトコルに合わせて
//    calcCRC8() の中身と FRAME_CRC_SIZE を差し替えてください。
//=====================================================================
#define FRAME_CRC_SIZE      1u
#define FRAME_HEADER_SIZE   2u   // コマンド(1) + データ長(1)
#define FRAME_MIN_SIZE      (FRAME_HEADER_SIZE + FRAME_CRC_SIZE)   // データ長=0でも成立する最小サイズ
#define FRAME_MAX_SIZE      64u  // 1フレームの最大サイズ(rxq_バッファ等に合わせて調整)

class Ci2cRoleSwitcher
{
public:
    // 受信バイトリングバッファ(ISRが積む生バイト列)
    struct Ring {
        uint8_t buf[256];
        volatile uint16_t head;
        volatile uint16_t tail;
    };

    // 1フレーム分の「開始位置・長さ」だけを保持する軽量キュー
    // 実データはrxq_に残したまま、位置情報だけをここに積む
    struct RxFrameInfo {
        uint16_t start;
        uint16_t length;
    };

    struct FrameRing {
        RxFrameInfo frame[16];
        volatile uint16_t head;
        volatile uint16_t tail;
    };

    struct SlaveTx {
        uint8_t *addr;
        uint16_t len;
        uint16_t counter;
    };

    enum Role { Controller, Target };

    enum RX_CMD_TYPE
    {
        RX_CMD_NONE = 0,
        RX_CMD_READ,
        RX_CMD_WRITE,
        RX_CMD_UNKNOWN
    };

    Ci2cRoleSwitcher(uint32_t base);

    // 直接送信
    inline void setDirectSend()
    {
        uint16_t data = 0xFF;
        if(txAddr.counter < txAddr.len)
        {
            data = txAddr.addr[txAddr.counter];
            HWREGH(base_ + I2C_O_DXR) = data;
            txAddr.counter++;
            if(txAddr.counter == txAddr.len)
            {
                txDone_ = true;
            }
        }
        else
        {
            HWREGH(base_ + I2C_O_DXR) = data;
        }
    }

    void configureFIFO(I2C_TxFIFOLevel txlvl, I2C_RxFIFOLevel rxlvl);

    void switchToTarget(uint16_t ownAddr);
    void switchToController();
    void i2cSendPolling(uint16_t addr, const uint8_t* data, uint16_t len);
    void switchToControllerReceive(uint16_t targetAddr, uint16_t len, bool repeat);

    bool enqueueTx(uint8_t b);
    bool dequeueRx(uint16_t b);

    Role role() const { return role_; }
    bool stopSeen() const { return stopDetected_; }
    bool nackSeen() const { return nack_; }
    void clearStopFlag() { stopDetected_ = false; }
    void clearNackFlag() { nack_ = false; }
    void clearTxDone() { txDone_ = false; }
    bool isTxDone() const { return txDone_; }

    // ISR から呼ぶ
    void onBasicISR();

    void prepareSlaveTxData(const uint8_t *src, uint16_t len);
    bool popRxByte(uint8_t& out);

    void setSlaveData(uint8_t * data, uint16_t len);

    // ---------------------------------------------------------------
    // フレーム単位でのポーリング取得 API(新設)
    //   ・STOPで確定した1フレームを取り出す
    //   ・長さフィールドの整合性チェック + CRCチェックを内部で実施
    //   ・不正フレームは自動的に読み捨てて false を返す
    //     (呼び出し側は false でもループを継続してよい。
    //      次のフレームがあればさらに取り出せる)
    // ---------------------------------------------------------------
    bool PopFrame(uint8_t *dst, uint16_t &len);

    void setSlaveDataParam(uint8_t * data, uint16_t len);
    void setSlaveDataVersion(uint8_t * data, uint16_t len);
    void setSlaveDataMode(uint8_t * data, uint16_t len);
    uint16_t getI2cHeadCnt();
    uint16_t getI2cTailCnt();

    // 統計(任意・デバッグ用): 破棄したフレーム数
    uint32_t droppedFrameCount() const { return droppedFrameCount_; }

private:
    void resetFIFO();
    void clearInterruptFlags(uint16_t status);
    void waitBusIdle();
    void clearFlags();
    void disableI2cInterrupts();
    void getFIFOData();
    bool i2cWaitStop(uint32_t base, uint32_t timeout);
    static uint8_t calcCRC8(const uint8_t *data, uint16_t len);

private:
    uint32_t base_;
    Role role_;
    I2C_AddressMode addrMode_;
    I2C_TxFIFOLevel txLvl_;
    I2C_RxFIFOLevel rxLvl_;
    volatile bool arbLost_;
    volatile bool stopIssued_;
    volatile bool stopDetected_;
    volatile bool nack_;
    volatile bool txDone_;
    volatile bool rx_stop_;
    volatile uint16_t rx_tail_;

    // 受信フレーム管理用
    volatile uint8_t rxByteCount_;      // 現在受信中のトランザクションのバイト数
    volatile uint16_t frameStart_;      // 現在受信中フレームの rxq_ 上の開始位置
    volatile bool readPending_;
    volatile uint8_t readPendingCmd_;
    volatile bool readTransaction_;
    volatile RX_CMD_TYPE rxCmdType_;
    uint32_t droppedFrameCount_;

    Ring rxq_;          // 受信バイトの実データを保持するリングバッファ
    FrameRing frameq_;  // STOPで確定したフレームの位置情報キュー

    SlaveTx txAddr;         // 現在送信対象(READ要求に応じて切替)
    SlaveTx txParamAddr;    // パラメータ送信分
    SlaveTx txVersionAddr;  // バージョン送信分
    SlaveTx txModeAddr;     // モード送信分
};

#endif /* PERIPHERAL_I2C_CI2CROLESWITCHER_HPP_ */
