// Ci2cRoleSwitcher_FrameRing.hpp
// Sample extension (illustrative) - integrate into your existing class.

struct RxFrameInfo
{
    uint16_t start;
    uint16_t length;
};

struct FrameRing
{
    RxFrameInfo frame[16];
    volatile uint16_t head;
    volatile uint16_t tail;
};

bool PopFrame(uint8_t *dst, uint16_t &len);
