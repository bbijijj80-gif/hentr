#ifndef LOGO_H
#define LOGO_H
#include <stdint.h>

/* Draws a stylized recreation of the hentrOS badge: a hatched center
 * blob with eight swirling petals connected by radial strokes, in the
 * spirit of the project's hand-drawn yellow-marker logo sketch. Uses
 * only the gfx.c primitives, so it works from both the bootloader (on
 * the raw framebuffer) and the kernel (on the desktop). */
void draw_hentros_logo(int cx, int cy, int scale, uint32_t color);

/* Particle/"dissolve" variant of the same logo: build_hentros_logo_points()
 * samples the logo's shapes as discrete dots (returns how many it wrote,
 * capped at maxPoints), and draw_logo_particles() renders them animated
 * between a scattered cloud (phase1000 = 0) and the assembled logo
 * (phase1000 = 1000). */
typedef struct { int x, y; } LogoPoint;

int build_hentros_logo_points(int cx, int cy, int scale, LogoPoint *out, int maxPoints);
void draw_logo_particles(const LogoPoint *pts, int count, int cx, int cy, int phase1000, uint32_t color);

#endif
