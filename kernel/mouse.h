#ifndef MOUSE_H
#define MOUSE_H
#include <stdint.h>

void mouse_init(void);
/* Polls the PS/2 controller; returns 1 and fills dx/dy/buttons if a
 * full packet was received, otherwise returns 0. */
int mouse_poll(int *dx, int *dy, uint8_t *buttons);

void keyboard_init(void);
/* Returns a raw scancode if one is pending, else 0. */
uint8_t keyboard_poll(void);

#endif
