//
// i_ps3video.c -- PS3 native video (RSX) for Crispy Doom.
//
// The public I_* entry points of Crispy Doom's i_video.h. The RSX itself
// (init, triple buffering, flips) is ps3_gpu.c since 1.4, and the frames
// are put together by i_ps3gpu.c: the 8 bit screen through the palette,
// over the GPU renderer's 3D view when there is one.
//

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <limits.h>

#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <sys/systime.h>

#include "crispy.h"
#include "config.h"
#include "deh_str.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "i_ps3gpu.h"
#include <stdarg.h>
#include <stdio.h>

int SCREENWIDTH, SCREENHEIGHT, SCREENHEIGHT_4_3;
int NONWIDEWIDTH;
int WIDESCREENDELTA;

pixel_t *I_VideoBuffer = NULL;

static boolean initialized = false;
static byte cached_palette[768];

// [PS3] The RSX is ps3_gpu.c since 1.4 (i_ps3gpu.c draws the frames):
// the 8 bit screen goes up through the palette and is scaled by the 3D
// unit, with the GPU renderer's view under it when there is one.

// [PS3] Share of the display the picture may use, in percent, to make
// up for TVs that crop the edges (overscan). Settable from the menu
// ("TV Screen Fit") and saved as ps3_screen_fit. 90 is what 1.0 had
// hardwired; 100 fills a monitor edge to edge.
int ps3_screen_fit = PS3_SCREEN_FIT_DEFAULT;

extern void PS3_Log(const char *fmt, ...);
extern void PS3_LogV(const char *fmt, ...);
extern void PS3_OSK_Update(void);

void PS3_GPU_Log(const char *fmt, ...)
{
    char buf[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    PS3_Log("%s", buf);
}

void
PS3_RSX_Init(void)
{
    // The launcher brings the RSX up before D_DoomMain so it can draw the
    // WAD picker with no engine available; I_InitGraphics calls this
    // again afterwards. GPU_Init is idempotent.
    if (GPU_Init() != 0)
        I_Error("PS3 video: the RSX didn't start (see the log)");
}

static void
PS3_RSX_Shutdown(void)
{
    GPU_Shutdown();
}

// The launcher's way out to the XMB (i_ps3launcher.c): it brought RSX
// up itself, before any of the engine's exit handlers existed.
void
PS3_RSX_Release(void)
{
    PS3_RSX_Shutdown();
}

// The launcher's 32 bit picture. Its lines are square pixels until the
// engine's aspect correction is known.
void
PS3_RSX_Present(const u32 *src, int src_w, int src_h)
{
    int aspect_h = (SCREENHEIGHT_4_3 > 0 && src_h == SCREENHEIGHT) ? SCREENHEIGHT_4_3 : src_h;

    I_GPU_Present32(src, src_w, src_h, src_w, aspect_h);
}

// --- Crispy Doom i_video.h contract ---

void I_GetScreenDimensions(void)
{
    // Must honor crispy->hires: the engine shifts every coordinate by
    // it (see V_CopyRect's "srcx <<= crispy->hires"), so hardcoding
    // 320x200 while hires is 1 makes every blit overrun the buffer
    // ("Bad V_CopyRect"). With hires=1 this yields the 640x400 internal
    // render we want, upscaled to 720p by the RSX blit.
    SCREENWIDTH = ORIGWIDTH << crispy->hires;
    SCREENHEIGHT = ORIGHEIGHT << crispy->hires;

    NONWIDEWIDTH = SCREENWIDTH;

    SCREENHEIGHT_4_3 = (aspect_ratio_correct == 1)
                     ? (6 * SCREENHEIGHT / 5)
                     : SCREENHEIGHT;

    // Widescreen widens the render itself and offsets the HUD by
    // WIDESCREENDELTA; PS3_RSX_Present then fits it at its own aspect.
    // Same maths as the SDL backend, minus the display-mode probe --
    // the PS3 output is whatever videoGetResolution reported and the
    // ratio is picked by the user, not detected.
    //
    // It only makes sense with aspect correction on: without it a
    // "line" is not 1.2 pixels tall and the ratio maths is meaningless.
    if (crispy->widescreen && aspect_ratio_correct == 1)
    {
        int w = 16, h = 10;

        switch (crispy->widescreen)
        {
            case RATIO_16_10: w = 16; h = 10; break;
            case RATIO_16_9:  w = 16; h = 9;  break;
            case RATIO_21_9:  w = 21; h = 9;  break;
            default: break;
        }

        SCREENWIDTH = w * SCREENHEIGHT_4_3 / h;

        // Must stay a multiple of 4, and never exceed MAXWIDTH -- the
        // engine's buffers are sized from it.
        SCREENWIDTH = (SCREENWIDTH + (crispy->hires ? 0 : 3)) & (int) ~3;
        SCREENWIDTH = MIN(SCREENWIDTH, MAXWIDTH);
    }

    WIDESCREENDELTA = ((SCREENWIDTH - NONWIDEWIDTH) >> crispy->hires) / 2;
}

// [crispy] intermediate gamma levels -- same table crispy-doom's own
// i_video.c builds, needed because R_InitData() (engine code, common to
// every platform) calls I_SetGammaTable() directly and expects this
// table to exist and be populated by the time I_SetPalette runs.
byte gamma2table[18][256];

static const float gammalevels[9] =
{
    0.50f, 0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f,
};

void I_SetGammaTable(void)
{
    int i, j, k;

    for (i = 0; i < 9; ++i)
    {
        for (j = 0; j < 256; ++j)
        {
            gamma2table[i][j] = (byte)(pow(j / 255.0, 1.0 / gammalevels[i]) * 255.0 + 0.5);
        }
    }

    for (i = 9, k = 0; i < 18 && k < 5; i += 2, k++)
    {
        memcpy(gamma2table[i], gammatable[k], 256);
    }

    for (i = 10, k = 0; i < 18 && k < 4; i += 2, k++)
    {
        for (j = 0; j < 256; j++)
        {
            gamma2table[i][j] = (gammatable[k][j] + gammatable[k + 1][j]) / 2;
        }
    }
}

typedef struct { byte r, g, b, a; } ps3color_t;
static ps3color_t palette[256];
static byte gpu_palette[768];

void I_SetPalette(byte *doompalette)
{
    memcpy(cached_palette, doompalette, sizeof(cached_palette));

    for (int i = 0; i < 256; i++)
    {
        // Zero out the bottom two bits of each channel (PC VGA-DAC
        // precision, same as original), gamma-corrected.
        palette[i].a = 0xFFU;
        palette[i].r = gamma2table[crispy->gamma][*doompalette++] & ~3;
        palette[i].g = gamma2table[crispy->gamma][*doompalette++] & ~3;
        palette[i].b = gamma2table[crispy->gamma][*doompalette++] & ~3;

        gpu_palette[i * 3] = palette[i].r;
        gpu_palette[i * 3 + 1] = palette[i].g;
        gpu_palette[i * 3 + 2] = palette[i].b;
    }

    I_GPU_SetPalette(gpu_palette);
}

// Given an RGB value, find the closest matching palette index.
int I_GetPaletteIndex(int r, int g, int b)
{
    int best, best_diff, diff;
    int i;

    best = 0; best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
        diff = (r - palette[i].r) * (r - palette[i].r)
             + (g - palette[i].g) * (g - palette[i].g)
             + (b - palette[i].b) * (b - palette[i].b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

void I_ReadScreen(pixel_t *scr)
{
    memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT * sizeof(*scr));
}

// [crispy] take screenshot of the rendered image -- simplified vs the
// original (no aspect-ratio-correct/integer-scaling crop handling,
// since we don't support those options in this first pass): just the
// raw internal buffer, converted straight to RGB via the current
// palette.
void I_RenderReadPixels(byte **data, int *w, int *h, int *p)
{
    static byte *rgb_buffer = NULL;
    int x, y;

    free(rgb_buffer);
    rgb_buffer = malloc(SCREENWIDTH * SCREENHEIGHT * 3);

    for (y = 0; y < SCREENHEIGHT; y++)
    {
        for (x = 0; x < SCREENWIDTH; x++)
        {
            byte idx = I_VideoBuffer[y * SCREENWIDTH + x];
            byte *dst = rgb_buffer + (y * SCREENWIDTH + x) * 3;
            dst[0] = palette[idx].r;
            dst[1] = palette[idx].g;
            dst[2] = palette[idx].b;
        }
    }

    *data = rgb_buffer;
    *w = SCREENWIDTH;
    *h = SCREENHEIGHT;
    *p = SCREENWIDTH * 3;
}

void I_InitGraphics(void)
{
    byte *doompal;

    PS3_Log("I_InitGraphics: start");
    I_GetScreenDimensions();

    V_Init();

    I_VideoBuffer = malloc(SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
    if (!I_VideoBuffer)
        I_Error("I_InitGraphics: failed to allocate I_VideoBuffer");

    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
    V_RestoreBuffer();

    doompal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_CACHE);
    I_SetPalette(doompal);

    PS3_RSX_Init();

    initialized = true;

    I_AtExit(I_ShutdownGraphics, true);
    PS3_Log("I_InitGraphics: complete");
}

void I_ReInitGraphics(int reinit)
{
    // Called through crispy->post_rendering_hook when the user changes
    // the aspect ratio or high-resolution rendering. Only the
    // framebuffer half of upstream's version applies here: everything
    // it does under REINIT_RENDERER and REINIT_TEXTURES is SDL surface
    // and texture juggling. RSX needs nothing -- ps3_gpu.c's screen
    // images are sized for MAXWIDTH up front, and i_ps3gpu.c takes every
    // dimension from SCREENWIDTH/SCREENHEIGHT, so a wider render just
    // works.
    if (reinit & REINIT_FRAMEBUFFERS)
    {
        byte *newbuffer;

        I_GetScreenDimensions();

        V_Init();

        newbuffer = malloc(SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
        if (newbuffer == NULL)
        {
            // Keep the old buffer rather than leaving the engine with
            // none: the picture stays at the previous size, which is
            // survivable, whereas a NULL here is not.
            PS3_Log("I_ReInitGraphics: alloc failed for %dx%d, keeping old",
                    SCREENWIDTH, SCREENHEIGHT);
            return;
        }

        free(I_VideoBuffer);
        I_VideoBuffer = newbuffer;
        memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));

        V_RestoreBuffer();

        PS3_LogV("I_ReInitGraphics: now %dx%d, widescreendelta=%d",
                SCREENWIDTH, SCREENHEIGHT, WIDESCREENDELTA);
    }
}

void I_ShutdownGraphics(void)
{
    if (!initialized)
        return;

    PS3_RSX_Shutdown();
    free(I_VideoBuffer);
    I_VideoBuffer = NULL;
    initialized = false;
}

void I_FinishUpdate(void)
{
    // Delivers any pending sysutil event (PS button, XMB "Quit Game")
    // to PS3_SysutilCallback in i_main.c. Registering the callback is
    // not enough on its own -- nothing fires until something pumps the
    // queue, and this is the one function that runs every frame.
    sysUtilCheckCallback();

    // Moves the XMB keyboard along; its events arrive through the call
    // above (i_ps3input.c).
    PS3_OSK_Update();

    if (!initialized)
        return;

    I_GPU_Present(I_VideoBuffer);
}

void I_UpdateNoBlit(void)
{
    // no-op: nothing extra to sync outside I_FinishUpdate on this port.
}

void I_StartFrame(void)
{
}

void I_StartTic(void)
{
    I_UpdateJoystick();
}

void I_CheckIsScreensaver(void)
{
}

void I_SetGrabMouseCallback(grabmouse_callback_t func)
{
    (void)func;
}

void I_DisplayFPSDots(boolean dots_on)
{
    (void)dots_on;
}

void I_BindVideoVariables(void)
{
    M_BindIntVariable("ps3_screen_fit", &ps3_screen_fit);
    M_BindIntVariable("ps3_renderer", &crispy_gpu_renderer);
    M_BindIntVariable("ps3_texture_filter", &crispy_gpu_texfilter);
}

void I_InitWindowTitle(void)
{
}

void I_InitWindowIcon(void)
{
}

void I_GraphicsCheckCommandLine(void)
{
}

void I_ToggleFullScreen(void)
{
}

void I_ToggleVsync(void)
{
}
