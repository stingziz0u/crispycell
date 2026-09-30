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

#include <string.h>

#include <sys/memory.h>
#include <sysutil/osk.h>
#include <sysutil/sysutil.h>

#include "doomtype.h"
#include "i_input.h"
#include "m_misc.h"

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

// Upstream's SDL text-input hooks stay no-ops: the menus that take text
// open the XMB keyboard explicitly (PS3_OSK_Start below) where it is
// needed, which is only Strife's character / savegame names.

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

// ---------------------------------------------------------------------
// XMB on-screen keyboard
// ---------------------------------------------------------------------
//
// The system keyboard, the one the browser and PSN use. The call
// sequence, the parameters and the 8 MB memory container are the ones
// Apollo Save Tool uses (source/dialog.c), a PSL1GHT homebrew that runs
// this on real hardware -- not guessed.
//
// It is asynchronous: oskLoadAsync opens it and returns at once, the
// system draws it over our frames, and the result arrives as sysutil
// events. Those are only delivered from sysUtilCheckCallback(), which
// i_ps3video.c calls once per frame -- so the game has to keep drawing
// while the keyboard is up (the menu does), and PS3_OSK_Update runs
// right after that call to move the state along:
//
//   INPUT_ENTERED  -> oskGetInputText (text so far)
//   INPUT_CANCELED -> oskAbort + oskUnloadAsync, result = cancel
//   DONE           -> oskUnloadAsync, result = ok
//   UNLOADED       -> read the text, free the container, finished
//
// The callback goes on sysutil slot 1; slot 0 is i_main.c's exit
// handler and stays untouched.

extern void PS3_Log(const char *fmt, ...);
extern void PS3_LogV(const char *fmt, ...);

#define OSK_MAX_TEXT      64
#define OSK_CONTAINER_MB  8

typedef enum
{
    OSK_STATE_IDLE,
    OSK_STATE_OPEN,
    OSK_STATE_DONE_OK,
    OSK_STATE_DONE_CANCEL
} osk_state_t;

static osk_state_t osk_state = OSK_STATE_IDLE;
static volatile int osk_event = 0;      // written by the callback
static volatile int osk_unloaded = 0;   // written by the callback
static int osk_action = 0;              // 1 = confirmed, -1 = canceled
static sys_mem_container_t osk_container;
static oskCallbackReturnParam osk_return;

static u16  osk_title[64];
static u16  osk_start[OSK_MAX_TEXT + 1];
static u16  osk_input[OSK_MAX_TEXT + 1];
static char osk_text[OSK_MAX_TEXT + 1];

static void OSK_Callback(u64 status, u64 param, void *userdata)
{
    (void) param;
    (void) userdata;

    switch ((u32) status)
    {
        case SYSUTIL_OSK_INPUT_ENTERED:
        case SYSUTIL_OSK_INPUT_CANCELED:
        case SYSUTIL_OSK_DONE:
            osk_event = (int) status;
            break;

        case SYSUTIL_OSK_UNLOADED:
            osk_unloaded = 1;
            break;

        default:
            break;
    }
}

// Names are plain ASCII -- the game font has nothing else.
static void ToUTF16(const char *src, u16 *dst, int max)
{
    int i;

    for (i = 0; i < max && src[i] != '\0'; i++)
    {
        dst[i] = (unsigned char) src[i];
    }

    dst[i] = 0;
}

// Keeps what the game can print. The keyboard opens on its full-width
// alphabet panel (as in Apollo), which types U+FF01..U+FF5E rather than
// ASCII; those map back by a fixed offset. Anything else is dropped.
static void FromUTF16(const u16 *src, char *dst, int max)
{
    int n = 0;

    for (; *src != 0 && n < max; src++)
    {
        unsigned c = *src;

        if (c >= 0xff01 && c <= 0xff5e)
        {
            c -= 0xfee0;
        }
        else if (c == 0x3000)
        {
            c = ' ';
        }

        if (c >= 0x20 && c < 0x7f)
        {
            dst[n++] = (char) c;
        }
    }

    dst[n] = '\0';
}

static void OSK_Release(void)
{
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1);
    sysMemContainerDestroy(osk_container);
}

//
// Opens the keyboard. Returns false when it could not be opened (the
// reason is logged); the caller then has to carry on without it.
//
boolean PS3_OSK_Start(const char *title, const char *initial, int maxlen)
{
    oskParam param;
    oskInputFieldInfo info;
    oskPoint pos = { 0.0f, 0.0f };
    s32 rc;

    if (osk_state == OSK_STATE_OPEN)
    {
        return false;
    }

    if (maxlen > OSK_MAX_TEXT)
    {
        maxlen = OSK_MAX_TEXT;
    }

    rc = sysMemContainerCreate(&osk_container, OSK_CONTAINER_MB * 1024 * 1024);
    if (rc != 0)
    {
        PS3_Log("OSK: sysMemContainerCreate(%d MB) failed: 0x%x",
                OSK_CONTAINER_MB, (unsigned) rc);
        return false;
    }

    memset(osk_input, 0, sizeof(osk_input));
    ToUTF16(title, osk_title, 63);
    ToUTF16(initial, osk_start, maxlen);

    memset(&info, 0, sizeof(info));
    info.message   = osk_title;
    info.startText = osk_start;
    info.maxLength = maxlen;

    memset(&osk_return, 0, sizeof(osk_return));
    osk_return.res = OSK_NO_TEXT;
    osk_return.len = OSK_MAX_TEXT;
    osk_return.str = osk_input;

    memset(&param, 0, sizeof(param));
    param.controlPoint   = pos;
    param.firstViewPanel = OSK_PANEL_TYPE_ALPHABET_FULL_WIDTH;
    param.allowedPanels  = OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL
                         | OSK_PANEL_TYPE_ENGLISH | OSK_PANEL_TYPE_URL;
    param.prohibitFlags  = OSK_PROHIBIT_RETURN;

    if ((rc = oskSetKeyLayoutOption(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL)) < 0
     || (rc = oskAddSupportLanguage(param.allowedPanels)) < 0
     || (rc = oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER
                             | OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER)) < 0
     || (rc = oskSetInitialInputDevice(OSK_DEVICE_PAD)) < 0)
    {
        PS3_Log("OSK: setup failed: 0x%x", (unsigned) rc);
        sysMemContainerDestroy(osk_container);
        return false;
    }

    osk_event = 0;
    osk_unloaded = 0;
    osk_action = 0;

    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT1, OSK_Callback, NULL);

    rc = oskLoadAsync(osk_container, &param, &info);
    if (rc < 0)
    {
        PS3_Log("OSK: oskLoadAsync failed: 0x%x", (unsigned) rc);
        OSK_Release();
        return false;
    }

    PS3_LogV("OSK: opened ('%s')", title);
    osk_state = OSK_STATE_OPEN;
    return true;
}

// Once per frame, right after sysUtilCheckCallback (i_ps3video.c).
void PS3_OSK_Update(void)
{
    if (osk_state != OSK_STATE_OPEN)
    {
        return;
    }

    if (!osk_unloaded)
    {
        switch (osk_event)
        {
            case SYSUTIL_OSK_INPUT_ENTERED:
                oskGetInputText(&osk_return);
                osk_event = 0;
                break;

            case SYSUTIL_OSK_INPUT_CANCELED:
                oskAbort();
                oskUnloadAsync(&osk_return);
                osk_event = 0;
                osk_action = -1;
                break;

            case SYSUTIL_OSK_DONE:
                if (osk_action != -1)
                    osk_action = 1;
                oskUnloadAsync(&osk_return);
                osk_event = 0;
                break;

            default:
                break;
        }
        return;
    }

    // Unloaded: the system is done with the buffers and the container.
    FromUTF16(osk_input, osk_text, OSK_MAX_TEXT);
    OSK_Release();

    if (osk_return.res == OSK_OK && osk_action == 1)
    {
        osk_state = OSK_STATE_DONE_OK;
        PS3_LogV("OSK: entered '%s'", osk_text);
    }
    else
    {
        osk_state = OSK_STATE_DONE_CANCEL;
        PS3_LogV("OSK: canceled (res=%d)", (int) osk_return.res);
    }
}

boolean PS3_OSK_Active(void)
{
    return osk_state == OSK_STATE_OPEN;
}

//
// Collects the result once: 1 = confirmed (text in out, possibly
// empty), -1 = canceled, 0 = nothing finished yet.
//
int PS3_OSK_TakeResult(char *out, size_t size)
{
    switch (osk_state)
    {
        case OSK_STATE_DONE_OK:
            osk_state = OSK_STATE_IDLE;
            M_StringCopy(out, osk_text, size);
            return 1;

        case OSK_STATE_DONE_CANCEL:
            osk_state = OSK_STATE_IDLE;
            return -1;

        default:
            return 0;
    }
}
