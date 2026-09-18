#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

/* Handed from the UEFI bootloader to the kernel after ExitBootServices. */
typedef struct {
    uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
} BootInfo;

#endif
