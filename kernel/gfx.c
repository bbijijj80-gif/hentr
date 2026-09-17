#include "gfx.h"
#include "font.h"

static uint32_t *fb;
static uint32_t fb_w, fb_h, fb_stride;

void gfx_init(BootInfo *info) {
    fb = info->framebuffer;
    fb_w = info->width;
    fb_h = info->height;
    fb_stride = info->pixels_per_scanline;
}

uint32_t gfx_width(void) { return fb_w; }
uint32_t gfx_height(void) { return fb_h; }

void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h) return;
    fb[y * fb_stride + x] = color;
}

uint32_t get_pixel(int x, int y) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h) return 0;
    return fb[y * fb_stride + x];
}

void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || (uint32_t)py >= fb_h) continue;
        int x0 = x, x1 = x + w;
        if (x0 < 0) x0 = 0;
        if ((uint32_t)x1 > fb_w) x1 = fb_w;
        uint32_t *row = &fb[py * fb_stride];
        for (int px = x0; px < x1; px++) row[px] = color;
    }
}

void draw_hline(int x, int y, int w, uint32_t color) { fill_rect(x, y, w, 1, color); }
void draw_vline(int x, int y, int h, uint32_t color) { fill_rect(x, y, 1, h, color); }

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

void draw_char(int x, int y, char c, uint32_t color, int scale) {
    const uint8_t *g = font_glyph(c);
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (1 << (FONT_W - 1 - col))) {
                if (scale == 1) {
                    put_pixel(x + col, y + row, color);
                } else {
                    fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

int text_width(const char *s, int scale) {
    int n = 0;
    while (*s++) n++;
    return n * (FONT_W + 1) * scale;
}

void draw_string(int x, int y, const char *s, uint32_t color, int scale) {
    int cx = x;
    while (*s) {
        draw_char(cx, y, *s, color, scale);
        cx += (FONT_W + 1) * scale;
        s++;
    }
}

void fill_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    int r0 = (top >> 16) & 0xFF, g0 = (top >> 8) & 0xFF, b0 = top & 0xFF;
    int r1 = (bottom >> 16) & 0xFF, g1 = (bottom >> 8) & 0xFF, b1 = bottom & 0xFF;
    for (int j = 0; j < h; j++) {
        int r = r0 + (r1 - r0) * j / (h > 1 ? h - 1 : 1);
        int g = g0 + (g1 - g0) * j / (h > 1 ? h - 1 : 1);
        int b = b0 + (b1 - b0) * j / (h > 1 ? h - 1 : 1);
        uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        draw_hline(x, y + j, w, color);
    }
}
