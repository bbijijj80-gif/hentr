#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

RtcTime rtc_read(void) {
    while (updating()) {}
    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t regB = cmos_read(0x0B);

    if (!(regB & 0x04)) { /* values are in BCD */
        sec = bcd2bin(sec);
        min = bcd2bin(min);
        hour = bcd2bin(hour & 0x7F) | (hour & 0x80);
    }
    if (!(regB & 0x02) && (hour & 0x80)) { /* 12-hour mode, PM flag set */
        hour = ((hour & 0x7F) + 12) % 24;
    }

    RtcTime t = { .hour = (uint8_t)(hour & 0x3F), .min = min, .sec = sec };
    return t;
}
