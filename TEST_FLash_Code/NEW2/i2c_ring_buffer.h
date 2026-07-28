//#############################################################################
// FILE:   i2c_ring_buffer.h
//
// I2Cスレーブ受信パケットを格納するためのリングバッファ。
// - push() : I2C割込み(ISR)側からのみ呼ぶ
// - pop()  : メインループ(ポーリング)側からのみ呼ぶ
//   -> Single Producer / Single Consumer 構成のため、head/tail操作に
//      排他制御(割込み禁止)は不要。ただしcount_の読み書き順序に注意する。
//#############################################################################
#ifndef I2C_RING_BUFFER_H
#define I2C_RING_BUFFER_H

#include <cstdint>
#include <cstddef>

//--- パケット仕様に合わせて調整 -----------------------------------------
#define I2C_PKT_MAX_DATA   32U   // 1パケットあたりの最大データ長(CRCバイトを含まない)
#define I2C_RING_DEPTH      8U   // リングバッファに保持できるパケット数

// フレーム形式
enum class I2cFrameFormat : uint8_t
{
    FORMAT_LEN     = 0,  // [CMD][LEN][DATA...][CRC][STOP]
    FORMAT_RESTART = 1   // [CMD][RESTART][DATA...][CRC][STOP]
};

// 1パケット分の受信データ
struct I2cPacket
{
    uint8_t        cmd;                     // コマンドバイト
    uint16_t       dataLen;                 // 実データ長(CRCバイトを除く)
    uint8_t        data[I2C_PKT_MAX_DATA];  // 実データ本体
    I2cFrameFormat format;                  // どちらの形式で受信したか
    bool           crcOk;                   // CRC照合結果
};

class I2cRingBuffer
{
public:
    I2cRingBuffer() : head_(0), tail_(0), count_(0), overrunCount_(0)
    {
    }

    // ISR側から呼ぶ。バッファフルの場合はfalseを返す(オーバーラン)。
    bool push(const I2cPacket &pkt)
    {
        if (count_ >= I2C_RING_DEPTH)
        {
            overrunCount_++;
            return false;
        }
        buffer_[head_] = pkt;
        head_ = static_cast<uint16_t>((head_ + 1U) % I2C_RING_DEPTH);
        count_++;
        return true;
    }

    // ポーリング側から呼ぶ。取得できたパケットがあればtrue。
    bool pop(I2cPacket &pkt)
    {
        if (count_ == 0U)
        {
            return false;
        }
        pkt = buffer_[tail_];
        tail_ = static_cast<uint16_t>((tail_ + 1U) % I2C_RING_DEPTH);
        count_--;
        return true;
    }

    uint16_t available() const { return count_; }
    uint32_t overrunCount() const { return overrunCount_; }

private:
    I2cPacket         buffer_[I2C_RING_DEPTH];
    volatile uint16_t head_;
    volatile uint16_t tail_;
    volatile uint16_t count_;
    volatile uint32_t overrunCount_;
};

#endif // I2C_RING_BUFFER_H
