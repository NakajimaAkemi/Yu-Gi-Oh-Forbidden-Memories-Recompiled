#ifndef MEMORIES_PC_PLATFORM_TOUCH_PAD_H
#define MEMORIES_PC_PLATFORM_TOUCH_PAD_H
#include <stdint.h>
#include "menu.h"

/* The on-screen pad: a controller drawn over the game for a device whose only
 * input is its screen (Android). It produces the same PS1 pad bits a keyboard
 * or a real pad does (controls.h), so nothing downstream knows the difference,
 * and it is painted into the overlay canvas the menu and the HUD share.
 *
 * It is not shown while a real pad is connected, and the whole module is
 * inert off Android. */

/* Whether the pad should be drawn and should take touches: a touchscreen
 * build with no controller plugged in. */
int TouchPad_Wanted(void);
/* Lay the buttons out for a window this size. Called whenever it changes. */
void TouchPad_Layout(int window_w, int window_h);
/* A finger arrived, moved or left, in window pixels. `down` is 0 when it
 * left. Returns nonzero when the bits or the lit buttons changed, which is
 * when the overlay has to be painted again. */
int TouchPad_Finger(int64_t finger, int down, int x, int y);
/* Every finger is gone (the window lost focus, the game was suspended). */
void TouchPad_Release(void);
/* The buttons held, as PS1 pad bits (CTRL_DEST_*). */
uint16_t TouchPad_Bits(void);
/* Paint the pad. The canvas is the transparent overlay. */
void TouchPad_Draw(MenuCanvas *canvas);
/* The rectangle TouchPad_Draw touches, so the platform can upload it; a zero
 * width or height when there is nothing to draw. */
void TouchPad_Bounds(int *x, int *y, int *w, int *h);

#endif
