#ifndef FONT_H_GUARD
#define FONT_H_GUARD
#include <stdint.h>

#define FONT_W 5
#define FONT_H 7

/* Returns a pointer to 7 bytes, each row's 5 low-order-relevant bits
 * (bit4 = leftmost column ... bit0 = rightmost column) describing the
 * glyph for an uppercase-only 5x7 bitmap font. Unsupported characters
 * render as blank space. */
const uint8_t *font_glyph(char c);

#endif
