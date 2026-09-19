//
// i_ps3joystick.c -- PS3 native joystick (DualShock 3) input.
//
// This is a joystick-only build: no mouse, no keyboard. The ioPad API
// usage (ioPadInit, ioPadGetInfo/.status[], ioPadGetData, button[]
// index layout) is copied verbatim from TyrQuakeCell's in_ps3.c, a
// confirmed-working reference -- not guessed.
//
// Unlike i_joystick.c (a generic "any SDL joystick" abstraction with a
// runtime-configurable virtual->physical button table), this targets
// exactly one known controller, so that whole indirection layer is
// gone: physical DualShock buttons map straight to virtual button
// indices below. Movement/turn is on the left stick, strafe/look is on
// the right stick -- a first-pass mapping, easy to retune once this is
// actually running on hardware, since Crispy's own sensitivity cvars
// (joystick_turn_sensitivity, etc.) still apply on top of this.
//

#include <stdio.h>
#include <string.h>

#include <io/pad.h>

#include "doomtype.h"
#include "d_event.h"
#include "i_joystick.h"
#include "i_system.h"
#include "m_config.h"
#include "m_fixed.h"
#include "i_timer.h"
#include "i_video.h"

// paddata.button[] index layout (see TyrQuakeCell's in_ps3.c):
//   [2],[3]  -- digital buttons, 16-bit bitmask split across two bytes
//   [4],[5]  -- right stick X, Y  (0-255, 128 = centered)
//   [6],[7]  -- left stick X, Y   (0-255, 128 = centered)
#define BUTTON_LEFT       32768
#define BUTTON_DOWN       16384
#define BUTTON_RIGHT      8192
#define BUTTON_UP         4096
#define BUTTON_START      2048
#define BUTTON_R3         1024
#define BUTTON_L3         512
#define BUTTON_SELECT     256
#define BUTTON_SQUARE     128
#define BUTTON_CROSS      64
#define BUTTON_CIRCLE     32
#define BUTTON_TRIANGLE   16
#define BUTTON_R1         8
#define BUTTON_L1         4
#define BUTTON_R2         2
#define BUTTON_L2         1

// Virtual button indices (bit position in ev.data1). Doom's config
// defaults (joybfire, joybuse, etc.) refer to these by number -- exact
// ergonomics are re-tunable later via config, this is a first pass.
#define VBTN_CROSS    0
#define VBTN_SQUARE   1
#define VBTN_CIRCLE   2
#define VBTN_TRIANGLE 3
#define VBTN_L1       4
#define VBTN_R1       5
#define VBTN_L2       6
#define VBTN_R2       7
#define VBTN_L3       8
#define VBTN_R3       9
#define VBTN_SELECT   10
#define VBTN_START    11
// The d-pad doubles as weapon switching in game. It still feeds data6
// for menu navigation; these are the in-game button bits. Must stay
// below MAX_JOY_BUTTONS (20) or g_game.c ignores them.
#define VBTN_DPAD_UP    12
#define VBTN_DPAD_DOWN  13
#define VBTN_DPAD_LEFT  14
#define VBTN_DPAD_RIGHT 15

int use_analog = 1;
int joystick_turn_sensitivity = 10;
int joystick_move_sensitivity = 10;
int joystick_look_sensitivity = 10;
int joystick_look_invert = 0;

static padInfo pad_info;
static padData pad_data;
static boolean pad_ready = false;

#define STICK_DEADZONE 24  // sticks rarely rest at exactly 128

void I_InitJoystick(void)
{
    ioPadInit(MAX_PADS);
    pad_ready = true;
    printf("I_InitJoystick: PS3 pad driver initialized.\n");
}

void I_ShutdownJoystick(void)
{
    pad_ready = false;
}

// Centers a raw 0-255 stick axis to a signed value, applies the dead
// zone, and scales to Doom's fixed-point range (-FRACUNIT..FRACUNIT).
static int ScaleStickAxis(unsigned char raw, boolean invert)
{
    int centered = (int)raw - 128;

    if (centered > -STICK_DEADZONE && centered < STICK_DEADZONE)
    {
        return 0;
    }

    if (invert)
    {
        centered = -centered;
    }

    // centered is in roughly -128..127; scale to -FRACUNIT..FRACUNIT
    return centered * FRACUNIT / 128;
}

static int GetDirectionalInput(unsigned pad_buttons, int leftx, int lefty)
{
    int dpad = JOY_DIR_NONE;
    int leftstick = JOY_DIR_NONE;

    if (pad_buttons & BUTTON_UP)    dpad |= JOY_DIR_UP;
    if (pad_buttons & BUTTON_DOWN)  dpad |= JOY_DIR_DOWN;
    if (pad_buttons & BUTTON_LEFT)  dpad |= JOY_DIR_LEFT;
    if (pad_buttons & BUTTON_RIGHT) dpad |= JOY_DIR_RIGHT;

    if (leftx > 0)      leftstick |= JOY_DIR_RIGHT;
    else if (leftx < 0) leftstick |= JOY_DIR_LEFT;
    if (lefty > 0)      leftstick |= JOY_DIR_DOWN;
    else if (lefty < 0) leftstick |= JOY_DIR_UP;

    return (dpad << DPAD_SHIFT) | (leftstick << LSTICK_SHIFT);
}

void I_UpdateJoystick(void)
{
    event_t ev;
    unsigned buttons;
    int leftx, lefty;
    int i;
    boolean found = false;

    if (!pad_ready)
    {
        return;
    }

    // Menus, the automap and weapon cycling are edge-triggered off a
    // level-triggered pad: without this the pad is polled every tic and
    // one held button reads as ~35 presses per second. m_menu.c,
    // am_map.c and g_game.c push joywait forward by 5 tics whenever they
    // act on a button; upstream gates the poll on it inside I_StartTic.
    if (joywait >= (unsigned int) I_GetTime())
    {
        return;
    }

    ioPadGetInfo(&pad_info);

    for (i = 0; i < MAX_PADS; i++)
    {
        if (pad_info.status[i])
        {
            ioPadGetData(i, &pad_data);
            found = true;
            break;
        }
    }

    if (!found)
    {
        return;
    }

    buttons = (pad_data.button[2] << 8) | (pad_data.button[3] & 0xff);

    ev.type = ev_joystick;
    ev.data1 = 0;
    if (buttons & BUTTON_CROSS)    ev.data1 |= 1 << VBTN_CROSS;
    if (buttons & BUTTON_SQUARE)   ev.data1 |= 1 << VBTN_SQUARE;
    if (buttons & BUTTON_CIRCLE)   ev.data1 |= 1 << VBTN_CIRCLE;
    if (buttons & BUTTON_TRIANGLE) ev.data1 |= 1 << VBTN_TRIANGLE;
    if (buttons & BUTTON_L1)       ev.data1 |= 1 << VBTN_L1;
    if (buttons & BUTTON_R1)       ev.data1 |= 1 << VBTN_R1;
    if (buttons & BUTTON_L2)       ev.data1 |= 1 << VBTN_L2;
    if (buttons & BUTTON_R2)       ev.data1 |= 1 << VBTN_R2;
    if (buttons & BUTTON_L3)       ev.data1 |= 1 << VBTN_L3;
    if (buttons & BUTTON_R3)       ev.data1 |= 1 << VBTN_R3;
    if (buttons & BUTTON_SELECT)   ev.data1 |= 1 << VBTN_SELECT;
    if (buttons & BUTTON_START)    ev.data1 |= 1 << VBTN_START;
    if (buttons & BUTTON_UP)       ev.data1 |= 1 << VBTN_DPAD_UP;
    if (buttons & BUTTON_DOWN)     ev.data1 |= 1 << VBTN_DPAD_DOWN;
    if (buttons & BUTTON_LEFT)     ev.data1 |= 1 << VBTN_DPAD_LEFT;
    if (buttons & BUTTON_RIGHT)    ev.data1 |= 1 << VBTN_DPAD_RIGHT;

    // Twin-stick layout: left stick moves, right stick aims.
    //   data2 = turn, data3 = forward/back, data4 = strafe, data5 = look
    // Signs are already right with invert=false: g_game.c does
    // "forward -= joyymove" and "look = -joylook", so stick-up (raw 0,
    // which centers negative) means forward and look up.
    leftx = ScaleStickAxis(pad_data.button[6], false);
    lefty = ScaleStickAxis(pad_data.button[7], false);

    ev.data2 = ScaleStickAxis(pad_data.button[4], false); // right X -> turn
    ev.data3 = lefty;                                     // left Y  -> move
    ev.data4 = leftx;                                     // left X  -> strafe
    ev.data5 = ScaleStickAxis(pad_data.button[5],
                              joystick_look_invert != 0); // right Y -> look
    ev.data6 = GetDirectionalInput(buttons, leftx, lefty);

    D_PostEvent(&ev);
}

void I_BindJoystickVariables(void)
{
    M_BindIntVariable("use_analog", &use_analog);
    M_BindIntVariable("joystick_turn_sensitivity", &joystick_turn_sensitivity);
    M_BindIntVariable("joystick_move_sensitivity", &joystick_move_sensitivity);
    M_BindIntVariable("joystick_look_sensitivity", &joystick_look_sensitivity);
    M_BindIntVariable("joystick_look_invert", &joystick_look_invert);
}
