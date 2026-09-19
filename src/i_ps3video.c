//
// i_ps3video.c -- PS3 native video (RSX) for Crispy Doom.
//
// The RSX init/present/shutdown sequence (triple buffering, GPU-
// accelerated scaled blit via rsxSetTransferScaleSurface, flip
// handling) is copied near-verbatim from TyrQuakeCell's vid_ps3.c, a
// confirmed-working reference on real hardware -- not guessed. Only
// the palette->ARGB conversion and the public I_* entry points are
// new, adapted to Crispy Doom's contract instead of Quake's vid.h.
//
// First-pass resolution: forced classic 320x200, no hires/widescreen
// yet -- gets something booting before tackling Crispy's runtime
// resolution options.
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

int SCREENWIDTH, SCREENHEIGHT, SCREENHEIGHT_4_3;
int NONWIDEWIDTH;
int WIDESCREENDELTA;

pixel_t *I_VideoBuffer = NULL;

static boolean initialized = false;
static u32 d_8to24table[256];
static byte cached_palette[768];

// --- RSX state (copied from vid_ps3.c) ---
#define PS3_NUM_BUFFERS 3

static void *rsx_io_buffer = NULL;
static gcmContextData *rsx_context = NULL;
static u32   rsx_offset[PS3_NUM_BUFFERS];
static void *rsx_mem[PS3_NUM_BUFFERS];
static int   rsx_current_buf = 0;
static int   rsx_display_w   = 0;
static int   rsx_display_h   = 0;
static int   rsx_pitch       = 0;
static boolean rsx_ready     = false;

static volatile u32 rsx_flip_queued = 0;
static volatile u32 rsx_flip_completed = 0;

static void *rsx_src_mem = NULL;
static u32   rsx_src_offset = 0;
static boolean gpu_scale_ready = false;

static u32 *argb_buffer_fallback = NULL; // only used if gpu_scale_ready fails

static void
PS3_RSX_FlipHandler(const u32 head)
{
    (void)head;
    rsx_flip_completed++;
}

static void
PS3_RSX_WaitForFreeBuffer(void)
{
    int waited = 0;
    while ((int)(rsx_flip_queued - rsx_flip_completed) > PS3_NUM_BUFFERS - 2)
    {
        sysUsleep(100);
        if (++waited > 20000)
        {
            rsx_flip_completed = rsx_flip_queued;
            break;
        }
    }
}

extern void PS3_Log(const char *fmt, ...);
extern void PS3_LogV(const char *fmt, ...);

void
PS3_RSX_Init(void)
{
    // The launcher brings RSX up before D_DoomMain so it can draw the
    // WAD picker with no engine available. I_InitGraphics calls this
    // again afterwards; without the guard that would memalign a second
    // IO buffer and call rsxInit twice, leaking the first context.
    if (rsx_ready)
    {
        PS3_Log("PS3_RSX_Init: already initialised, skipping");
        return;
    }

    PS3_Log("PS3_RSX_Init: start");
    rsx_io_buffer = memalign(1024 * 1024, 1024 * 1024);
    if (!rsx_io_buffer)
        I_Error("PS3 video: memalign failed for RSX IO buffer");

    s32 rc = rsxInit(&rsx_context, 0x10000, 1024 * 1024, rsx_io_buffer);
    if (rc != 0)
        I_Error("PS3 video: rsxInit failed: %d", (int)rc);

    videoState vstate;
    if (videoGetState(0, 0, &vstate) != 0)
        I_Error("PS3 video: videoGetState failed");

    u8 target_resolution = VIDEO_RESOLUTION_720;
    videoResolution res;
    if (videoGetResolution(target_resolution, &res) != 0)
    {
        target_resolution = vstate.displayMode.resolution;
        if (videoGetResolution(target_resolution, &res) != 0)
            I_Error("PS3 video: videoGetResolution failed");
    }

    rsx_display_w = res.width;
    rsx_display_h = res.height;
    rsx_pitch     = rsx_display_w * 4; // XRGB, 4 bytes/pixel

    videoConfiguration vconfig;
    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = target_resolution;
    vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch      = rsx_pitch;
    if (videoConfigure(0, &vconfig, NULL, 1) != 0)
        I_Error("PS3 video: videoConfigure failed");

    videoState wait_state;
    do
    {
        sysUsleep(10000);
        if (videoGetState(0, 0, &wait_state) != 0)
            break;
    } while (wait_state.state == 3);

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (int i = 0; i < PS3_NUM_BUFFERS; i++)
    {
        rsx_mem[i] = rsxMemalign(64, rsx_pitch * rsx_display_h);
        if (!rsx_mem[i])
            I_Error("PS3 video: rsxMemalign failed for buffer %d", i);
        rsxAddressToOffset(rsx_mem[i], &rsx_offset[i]);
        gcmSetDisplayBuffer(i, rsx_offset[i], rsx_pitch,
                             rsx_display_w, rsx_display_h);
    }

    rsx_flip_queued = 0;
    rsx_flip_completed = 0;
    gcmSetFlipHandler(PS3_RSX_FlipHandler);
    rsx_current_buf = 0;

    // Size for the widest render the engine can ever ask for, not for
    // whatever SCREENWIDTH happens to hold right now: the launcher pins
    // it to 640 before this runs, and switching to 21:9 later takes it
    // to 1120. PS3_RSX_Present memcpys src_w * src_h * 4 into here, so
    // an undersized buffer is an out-of-bounds write, not a glitch.
    // MAXWIDTH is ORIGWIDTH << 2 = 1280; at 4 bytes and 400 lines that
    // is 2 MiB, which is nothing next to the 256 MiB the console has.
    u32 src_pitch = MAXWIDTH * 4;
    rsx_src_mem = rsxMemalign(64, src_pitch * (ORIGHEIGHT << 1));
    if (rsx_src_mem)
    {
        rsxAddressToOffset(rsx_src_mem, &rsx_src_offset);
    }
    gpu_scale_ready = (rsx_src_mem != NULL);

    if (!gpu_scale_ready)
    {
        argb_buffer_fallback = malloc(SCREENWIDTH * SCREENHEIGHT * sizeof(u32));
        if (!argb_buffer_fallback)
            I_Error("PS3 video: failed to allocate CPU fallback ARGB buffer");
    }

    rsx_ready = true;
    PS3_Log("PS3_RSX_Init: complete, gpu_scale_ready=%d", gpu_scale_ready);
}

static void
PS3_RSX_Shutdown(void)
{
    if (!rsx_ready)
        return;

    gcmSetFlipHandler(NULL);
    for (int i = 0; i < PS3_NUM_BUFFERS; i++)
    {
        if (rsx_mem[i])
        {
            rsxFree(rsx_mem[i]);
            rsx_mem[i] = NULL;
        }
    }
    if (rsx_src_mem)
    {
        rsxFree(rsx_src_mem);
        rsx_src_mem = NULL;
    }
    if (rsx_io_buffer)
    {
        free(rsx_io_buffer);
        rsx_io_buffer = NULL;
    }
    free(argb_buffer_fallback);
    argb_buffer_fallback = NULL;

    rsx_ready = false;
}

void
PS3_RSX_Present(const u32 *src, int src_w, int src_h)
{
    if (!rsx_ready)
        return;

    PS3_RSX_WaitForFreeBuffer();

    int next = rsx_current_buf;
    u32 *dst = (u32 *)rsx_mem[next];

    // Fit the source into the safe area at its true aspect ratio,
    // rather than stretching it to fill. The engine renders with
    // non-square pixels: 400 lines stand for SCREENHEIGHT_4_3 (480 when
    // aspect correction is on), so the picture a 640x400 buffer
    // represents is 4:3, and a 852x400 widescreen buffer is 16:9.
    // Blitting either into a fixed 16:9 box was stretching the 4:3 case
    // about a third too wide.
    const float safe_area = 0.90f;
    int max_w = (int)(rsx_display_w * safe_area);
    int max_h = (int)(rsx_display_h * safe_area);
    int aspect_h = (SCREENHEIGHT_4_3 > 0) ? SCREENHEIGHT_4_3 : src_h;
    int out_w, out_h, off_x, off_y;

    // Pick whichever dimension runs out first, so the result is
    // pillarboxed on a 4:3 render and letterboxed if a source is ever
    // wider than the display.
    out_w = max_w;
    out_h = (int)((long long) out_w * aspect_h / src_w);

    if (out_h > max_h)
    {
        out_h = max_h;
        out_w = (int)((long long) out_h * src_w / aspect_h);
    }

    off_x = (rsx_display_w - out_w) / 2;
    off_y = (rsx_display_h - out_h) / 2;

    // Clear every frame now: with pillarboxing the bars are real
    // screen area, and a resolution or aspect change mid-game would
    // otherwise leave the old image framing the new one.
    memset(dst, 0, rsx_pitch * rsx_display_h);

    if (gpu_scale_ready)
    {
        if (src != (const u32 *)rsx_src_mem)
            memcpy(rsx_src_mem, src, src_w * src_h * 4);

        gcmTransferScale scale;
        memset(&scale, 0, sizeof(scale));
        scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
        scale.format     = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
        scale.operation  = GCM_TRANSFER_OPERATION_SRCCOPY;
        scale.clipX = off_x;
        scale.clipY = off_y;
        scale.clipW = out_w;
        scale.clipH = out_h;
        scale.outX  = off_x;
        scale.outY  = off_y;
        scale.outW  = out_w;
        scale.outH  = out_h;
        scale.ratioX = rsxGetFixedSint32((float)src_w / (float)out_w);
        scale.ratioY = rsxGetFixedSint32((float)src_h / (float)out_h);
        scale.inW    = src_w;
        scale.inH    = src_h;
        scale.pitch  = src_w * 4;
        scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
        scale.interp = GCM_TRANSFER_INTERPOLATOR_NEAREST;
        scale.offset = rsx_src_offset;
        scale.inX = 0;
        scale.inY = 0;

        gcmTransferSurface surface;
        memset(&surface, 0, sizeof(surface));
        surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
        surface.pitch  = rsx_pitch;
        surface.offset = rsx_offset[next];

        rsxSetTransferScaleSurface(rsx_context, &scale, &surface);
    }
    else
    {
        // CPU nearest-neighbour fallback, only if the RSX source
        // buffer failed to allocate at init.
        for (int y = 0; y < out_h; y++)
        {
            int sy = (y * src_h) / out_h;
            const u32 *sr = src + (sy * src_w);
            u32       *dr = dst + ((y + off_y) * rsx_display_w) + off_x;
            for (int x = 0; x < out_w; x++)
            {
                dr[x] = sr[(x * src_w) / out_w];
            }
        }
    }

    gcmSetFlip(rsx_context, next);
    rsxFlushBuffer(rsx_context);
    gcmSetWaitFlip(rsx_context);

    rsx_flip_queued++;
    rsx_current_buf = (rsx_current_buf + 1) % PS3_NUM_BUFFERS;
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

        // PS3 is big-endian: 0xAARRGGBB as a u32 literally matches
        // memory byte order [A][R][G][B], which is what
        // VIDEO_BUFFER_FORMAT_XRGB expects (same reasoning as
        // TyrQuakeCell's VID_SetPalette).
        d_8to24table[i] = ((u32)palette[i].a << 24) | ((u32)palette[i].r << 16)
                        | ((u32)palette[i].g << 8) | (u32)palette[i].b;
    }
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
    // and texture juggling. RSX needs nothing -- rsx_src_mem is sized
    // for MAXWIDTH up front, and PS3_RSX_Present takes every dimension
    // from its parameters, so a wider render just works.
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

    if (!initialized)
        return;

    u32 *convert_dst = gpu_scale_ready ? (u32 *)rsx_src_mem : argb_buffer_fallback;

    for (int y = 0; y < SCREENHEIGHT; y++)
    {
        const byte *src = I_VideoBuffer + y * SCREENWIDTH;
        u32        *dst = convert_dst + y * SCREENWIDTH;
        int x = 0;
        for (; x + 4 <= SCREENWIDTH; x += 4)
        {
            dst[x + 0] = d_8to24table[src[x + 0]];
            dst[x + 1] = d_8to24table[src[x + 1]];
            dst[x + 2] = d_8to24table[src[x + 2]];
            dst[x + 3] = d_8to24table[src[x + 3]];
        }
        for (; x < SCREENWIDTH; x++)
        {
            dst[x] = d_8to24table[src[x]];
        }
    }

    PS3_RSX_Present(convert_dst, SCREENWIDTH, SCREENHEIGHT);
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
