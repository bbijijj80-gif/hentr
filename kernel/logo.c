#include "logo.h"
#include "gfx.h"

/* Bhaskara I's sine approximation: integer-only, no libm needed in a
 * freestanding build. Returns sin(deg) scaled by 1000. */
static int isin1000(int deg) {
    deg = ((deg % 360) + 360) % 360;
    int sign = 1, x = deg;
    if (x > 180) { x -= 180; sign = -1; }
    int num = 4 * x * (180 - x);
    int den = 40500 - x * (180 - x);
    return sign * (1000 * num) / den;
}
static int icos1000(int deg) { return isin1000(deg + 90); }

static void put_thick(int x, int y, uint32_t color) {
    put_pixel(x, y, color);
    put_pixel(x + 1, y, color);
    put_pixel(x, y + 1, color);
    put_pixel(x + 1, y + 1, color);
}

/* A hand-drawn-looking swirl: a spiral arm winding out from the center. */
static void draw_swirl(int cx, int cy, int r0, int r1, int turns, uint32_t color) {
    int totalDeg = turns * 360;
    for (int t = 0; t <= totalDeg; t += 3) {
        int r = r0 + (r1 - r0) * t / totalDeg;
        int x = cx + r * icos1000(t) / 1000;
        int y = cy + r * isin1000(t) / 1000;
        put_thick(x, y, color);
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = dx > 0 ? dx : -dx;
    int ady = dy > 0 ? dy : -dy;
    if (ady > steps) steps = ady;
    if (steps == 0) { put_pixel(x0, y0, color); return; }
    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps;
        int y = y0 + dy * i / steps;
        put_pixel(x, y, color);
    }
}

/* The hatched center blob: a rounded diamond filled with diagonal strokes,
 * echoing the cross-hatched core of the original sketch. */
static void draw_hatched_core(int cx, int cy, int radius, uint32_t color) {
    for (int dy = -radius; dy <= radius; dy++) {
        int half = radius - (dy < 0 ? -dy : dy) / 2;
        int y = cy + dy;
        int x0 = cx - half, x1 = cx + half;
        if ((dy + radius) % 3 != 0) continue;
        draw_line(x0, y, x1, y - radius / 2, color);
    }
    /* outline */
    for (int t = 0; t < 360; t += 4) {
        int x = cx + radius * icos1000(t) / 1000;
        int y = cy + radius * isin1000(t) / 1200;
        put_pixel(x, y, color);
    }
}

void draw_hentros_logo(int cx, int cy, int scale, uint32_t color) {
    int petalDist = 11 * scale;
    int coreR = 5 * scale;

    for (int i = 0; i < 8; i++) {
        int deg = i * 45;
        int px = cx + petalDist * icos1000(deg) / 1000;
        int py = cy + petalDist * isin1000(deg) / 1000;
        int ex = cx + (coreR + 1) * icos1000(deg) / 1000;
        int ey = cy + (coreR + 1) * isin1000(deg) / 1000;
        draw_line(ex, ey, px, py, color);
        draw_swirl(px, py, 1, 3 * scale, 2, color);
    }

    draw_hatched_core(cx, cy, coreR, color);
}
