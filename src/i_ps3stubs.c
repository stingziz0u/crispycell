//
// i_ps3stubs.c -- misc globals/no-ops that Crispy's original i_video.c
// and i_sdlmusic.c/i_sdlsound.c used to provide. These are variables
// the shared game code (menus, am_map, gusconf) reads/writes directly,
// or trivial platform hooks with nothing to do on PS3 (no window, no
// GUS/Timidity MIDI support, no libsamplerate resampling since audio
// isn't real PCM yet -- see i_pcsound.c).
//

#include <stdarg.h>
#include <stdio.h>

#include "doomtype.h"
#include "i_system.h"
#include "i_timer.h"
#include "m_fixed.h"

// Writes to a log file on dev_hdd0 so we have visibility into crashes on
// real hardware, where printf output goes nowhere visible. Opens/closes
// and flushes on every call (deliberately not buffered/kept open) so
// that if the game crashes hard right after a call, the line we just
// wrote is still on disk -- same pattern as TyrQuakeCell's PS3_Log.
#define PS3_LOG_PATH_A PS3_USRDIR "/crispy_log.txt"
#define PS3_LOG_PATH_B "/dev_hdd0/tmp/crispy_log.txt"

static void PS3_LogTo(const char *path, const char *fmt, va_list argptr_orig)
{
    va_list argptr;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    va_copy(argptr, argptr_orig);
    vfprintf(f, fmt, argptr);
    va_end(argptr);
    fprintf(f, "\n");
    fclose(f);
}

void PS3_Log(const char *fmt, ...)
{
    va_list argptr;
    va_start(argptr, fmt);
    PS3_LogTo(PS3_LOG_PATH_A, fmt, argptr);
    va_end(argptr);

    va_start(argptr, fmt);
    PS3_LogTo(PS3_LOG_PATH_B, fmt, argptr);
    va_end(argptr);
}

// Runs as part of C runtime static initialization, before main() gets
// control at all -- the earliest hook pure C code can register without
// touching crt0/the linker script directly. If this line never shows
// up in either log path, the .self isn't reaching user code at all
// (a loading/format/signing problem, not a logic bug in our code).
__attribute__((constructor))
static void PS3_EarliestLog(void)
{
    PS3_Log("=== PS3_EarliestLog: C runtime constructor reached ===");
}

boolean screenvisible = true;
boolean screensaver_mode = false;
int usemouse = 0; // no mouse in this build
int png_screenshots = 1;
int aspect_ratio_correct = true;
int smooth_pixel_scaling = false; // no smoothing/upscale shader on this port
int force_software_renderer = true; // software renderer -> RSX blit, always
int vanilla_keyboard_mapping = true;
unsigned int joywait = 0;

char *timidity_cfg_path = "";

void I_InitTimidityConfig(void)
{
    // No GUS/Timidity MIDI support on this port.
}

int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

fixed_t fractionaltic;

void I_UpdateFracTic(void)
{
    fractionaltic = I_GetFracRealTime();
}

void I_StartDisplay(void)
{
    // No mouse to pump/read on this port.
}

void I_SetWindowTitle(const char *title)
{
}

void I_RegisterWindowIcon(const unsigned int *icon, int width, int height)
{
}
