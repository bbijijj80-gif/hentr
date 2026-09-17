#ifndef LOGO_H
#define LOGO_H
#include <stdint.h>

/* Draws a stylized recreation of the hentrOS badge: a hatched center
 * blob with eight swirling petals connected by radial strokes, in the
 * spirit of the project's hand-drawn yellow-marker logo sketch. Uses
 * only the gfx.c primitives, so it works from both the bootloader (on
 * the raw framebuffer) and the kernel (on the desktop). */
void draw_hentros_logo(int cx, int cy, int scale, uint32_t color);

#endif
