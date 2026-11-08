#pragma once
#include "taisei.h"

#ifdef __ANDROID__

void touch_controls_init(void);
void touch_controls_shutdown(void);
void touch_controls_draw(void);

// Whether the touch controls currently synthesize a press of the given
// SDL scancode. Used by gamekeypressed() to make the engine see touch input
// as "held", so player_fix_input() does not clear the input flags every frame.
bool touch_controls_key_pressed(int scancode);

#endif
