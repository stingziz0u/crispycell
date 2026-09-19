//
// i_ps3input.c -- PS3 native input stubs.
//
// Crispy's mouse/keyboard reading (i_input.c) is fully replaced: this
// build is joystick-only. The mouse config variables and the pure-math
// accel functions are kept so nothing elsewhere in the engine (config
// binding, g_game.c) needs to change -- they simply never receive real
// input, since nothing ever sets mousex/mousey away from zero.
//
// Real pad reading lives in i_ps3joystick.c, not here.
//

#include "doomtype.h"
#include "i_input.h"

int novert = 1;
float mouse_acceleration = 2.0;
int mouse_threshold = 10;
float mouse_acceleration_y = 1.0;
int mouse_threshold_y = 0;
int mouse_y_invert = 0;
int runcentering = 1;

double I_AccelerateMouse(int val)
{
    if (val < 0)
        return -I_AccelerateMouse(-val);

    if (val > mouse_threshold)
    {
        return (double)(val - mouse_threshold) * mouse_acceleration + mouse_threshold;
    }
    else
    {
        return val;
    }
}

double I_AccelerateMouseY(int val)
{
    if (val < 0)
        return -I_AccelerateMouseY(-val);

    if (val > mouse_threshold_y)
    {
        return (double)(val - mouse_threshold_y) * mouse_acceleration_y + mouse_threshold_y;
    }
    else
    {
        return val;
    }
}

// No on-screen keyboard / text entry on PS3 -- joystick-only build.

void I_StartTextInput(int x1, int y1, int x2, int y2)
{
}

void I_StopTextInput(void)
{
}

// Mouse config binding: nothing to bind since there is no mouse reader.
// Left as no-ops rather than removed, so callers in d_main.c/h2_main.c
// don't need PS3_BUILD conditionals of their own.

void I_BindInputVariables(void)
{
}

void I_BindStrifeInputVariables(void)
{
}
