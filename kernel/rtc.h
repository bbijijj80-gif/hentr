#ifndef RTC_H
#define RTC_H
#include <stdint.h>

typedef struct { uint8_t hour, min, sec; } RtcTime;
RtcTime rtc_read(void);

#endif
