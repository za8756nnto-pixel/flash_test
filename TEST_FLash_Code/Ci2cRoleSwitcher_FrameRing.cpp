// Ci2cRoleSwitcher_FrameRing.cpp
// Illustrative implementation.

static inline void frame_init(FrameRing *q)
{
    q->head = q->tail = 0;
}

static inline bool frame_push(FrameRing *q, uint16_t start, uint16_t length)
{
    uint16_t n = (q->head + 1) & 0x0F;
    if(n == q->tail) return false;
    q->frame[q->head].start = start;
    q->frame[q->head].length = length;
    q->head = n;
    return true;
}

static inline bool frame_pop(FrameRing *q, RxFrameInfo &f)
{
    if(q->head == q->tail) return false;
    f = q->frame[q->tail];
    q->tail = (q->tail + 1) & 0x0F;
    return true;
}

// STOP ISR example:
// frame_push(&frameq_, frameStart_, rxByteCount_);
// frameStart_ = rxq_.head;

bool Ci2cRoleSwitcher::PopFrame(uint8_t *dst, uint16_t &len)
{
    RxFrameInfo f;
    if(!frame_pop(&frameq_, f))
        return false;

    len = f.length;
    uint16_t pos = f.start;
    for(uint16_t i=0;i<len;i++)
    {
        dst[i] = rxq_.buf[pos];
        pos = (pos + 1) & 0xFF;
    }
    rxq_.tail = pos;
    return true;
}

// Polling:
// if(PopFrame(buf,len)){
//   // verify len >= minimum
//   // verify Length field matches frame size
//   // verify CRC
//   // parse
// }
