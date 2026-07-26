//#############################################################################
// FILE:   main.cpp
//
// I2Cスレーブ受信ドライバの使用例
//  - I2C周辺機能の初期化
//  - I2C割込みおよび1msタイマ割込みのPIE登録
//  - メインループでのリングバッファのポーリング処理
//
// 注意: I2CA_BASE / INT_I2CA / GPIOピン番号などは F280015x の
//       device_support ヘッダ(またはSysConfig生成コード)の定義に
//       合わせて書き換えること。ここでは典型的なC2000プロジェクト
//       構成を前提とした一般的な記述としている。
//#############################################################################
extern "C"
{
#include "driverlib.h"
#include "device.h"
}
#include "i2c_slave_driver.h"

// 1msタイマ割込みハンドラ(CPU Timer0を使用する例)
extern "C" __interrupt void cpuTimer0ISR(void)
{
    i2cSlaveTimeoutTick();

    CPUTimer_clearOverflowFlag(CPUTIMER0_BASE);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}

static void setupGpioForI2C(void)
{
    // SDA/SCLのピン番号は基板・デバイスに合わせて変更すること
    GPIO_setPinConfig(GPIO_28_I2CA_SDA);
    GPIO_setPinConfig(GPIO_29_I2CA_SCL);

    GPIO_setPadConfig(28, GPIO_PIN_TYPE_OD | GPIO_PIN_TYPE_PULLUP);
    GPIO_setPadConfig(29, GPIO_PIN_TYPE_OD | GPIO_PIN_TYPE_PULLUP);

    GPIO_setQualificationMode(28, GPIO_QUAL_ASYNC);
    GPIO_setQualificationMode(29, GPIO_QUAL_ASYNC);
}

static void setup1msTimer(void)
{
    // SYSCLK 100MHz を想定。実際のクロックはDevice_init()後のDEVICE_SYSCLK_FREQに合わせる。
    CPUTimer_setPeriod(CPUTIMER0_BASE, (DEVICE_SYSCLK_FREQ / 1000U) - 1U);
    CPUTimer_setPreScaler(CPUTIMER0_BASE, 0);
    CPUTimer_stopTimer(CPUTIMER0_BASE);
    CPUTimer_reloadTimerCounter(CPUTIMER0_BASE);
    CPUTimer_setEmulationMode(CPUTIMER0_BASE, CPUTIMER_EMULATIONMODE_STOPAFTERNEXTDECREMENT);
    CPUTimer_enableInterrupt(CPUTIMER0_BASE);

    Interrupt_register(INT_TIMER0, &cpuTimer0ISR);
    Interrupt_enable(INT_TIMER0);

    CPUTimer_startTimer(CPUTIMER0_BASE);
}

static void processPacket(const I2cPacket &pkt)
{
    // ここで受信済みパケットに対する実際のアプリ処理を行う。
    // 例: CMDごとの分岐処理
    if (!pkt.crcOk)
    {
        // CRC不一致パケット。ログ等に記録して破棄する運用も可。
        return;
    }

    switch (pkt.cmd)
    {
        case 0x01U:
            // 例: pkt.data[0..pkt.dataLen-1] を使った処理
            break;

        default:
            break;
    }
}

int main(void)
{
    Device_init();
    Device_initGPIO();

    Interrupt_initModule();
    Interrupt_initVectorTable();

    setupGpioForI2C();

    // I2C割込み(基本割込み: AAS/RRDY/SCD/NACK/ARBLOST)をPIEへ登録
    Interrupt_register(I2C_SLAVE_INT, &i2cSlaveBasicISR);
    Interrupt_enable(I2C_SLAVE_INT);

    g_i2cSlave.init(I2C_SLAVE_BASE);

    setup1msTimer();

    EINT;
    ERTM;

    for (;;)
    {
        I2cPacket pkt;
        while (g_i2cSlave.popPacket(pkt))
        {
            processPacket(pkt);
        }

        // 必要であれば以下でオーバーラン/エラー統計を監視できる
        // g_i2cSlave.overrunCount();
        // g_i2cSlave.crcErrorCount();
        // g_i2cSlave.lenMismatchCount();
        // g_i2cSlave.timeoutCount();
    }
}
