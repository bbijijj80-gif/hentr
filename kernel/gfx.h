#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include "../boot/bootinfo.h"

void gfx_init(BootInfo *info);
uint32_t gfx_width(void);
uint32_t gfx_height(void);

/* Copies the whole draw target (set up by gfx_init) to a real, possibly
 * differently-strided framebuffer in one pass. Drawing into an
 * off-screen buffer and presenting it like this avoids the tearing you
 * get from painting shapes directly onto a live-scanned-out framebuffer
 * with no vsync. */
void gfx_present(uint32_t *dst, uint32_t dst_stride);

void put_pixel(int x, int y, uint32_t color);
uint32_t get_pixel(int x, int y);
void fill_rect(int x, int y, int w, int h, uint32_t color);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void draw_hline(int x, int y, int w, uint32_t color);
void draw_vline(int x, int y, int h, uint32_t color);
void draw_char(int x, int y, char c, uint32_t color, int scale);
void draw_string(int x, int y, const char *s, uint32_t color, int scale);
int text_width(const char *s, int scale);
void fill_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

#endif
