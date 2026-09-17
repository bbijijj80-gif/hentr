#include "mouse.h"
#include "io.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & 0x02)) return;
}
static void ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & 0x01) return;
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}
static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}
static uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static void mouse_write(uint8_t data) {
    ps2_write_cmd(0xD4); /* next byte goes to the auxiliary (mouse) device */
    ps2_write_data(data);
}

void mouse_init(void) {
    ps2_write_cmd(0xA8); /* enable auxiliary device */

    ps2_write_cmd(0x20); /* read controller config byte */
    uint8_t cfg = ps2_read_data();
    cfg |= 0x02;  /* enable IRQ12 line status bit (used here just for polling readiness) */
    cfg &= ~0x20; /* enable mouse clock */
    ps2_write_cmd(0x60);
    ps2_write_data(cfg);

    mouse_write(0xF6); /* set defaults */
    ps2_read_data();
    mouse_write(0xF4); /* enable data reporting */
    ps2_read_data();
}

static uint8_t packet[3];
static int packet_idx = 0;

int mouse_poll(int *dx, int *dy, uint8_t *buttons) {
    int got = 0;
    while (inb(PS2_STATUS) & 0x01) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & 0x20)) { /* byte belongs to keyboard, not mouse: drain and skip */
            inb(PS2_DATA);
            continue;
        }
        uint8_t data = inb(PS2_DATA);
        if (packet_idx == 0 && !(data & 0x08)) continue; /* resync on bad first byte */
        packet[packet_idx++] = data;
        if (packet_idx == 3) {
            packet_idx = 0;
            *buttons = packet[0] & 0x07;
            int rawdx = packet[1] - ((packet[0] << 4) & 0x100);
            int rawdy = packet[2] - ((packet[0] << 3) & 0x100);
            *dx = rawdx;
            *dy = -rawdy; /* PS/2 Y axis is inverted relative to screen coordinates */
            got = 1;
            break;
        }
    }
    return got;
}

void keyboard_init(void) {
    /* Nothing extra needed; the controller already scans by default. */
}

uint8_t keyboard_poll(void) {
    if (inb(PS2_STATUS) & 0x01) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & 0x20)) { /* keyboard byte (AUX bit clear) */
            return inb(PS2_DATA);
        }
    }
    return 0;
}
