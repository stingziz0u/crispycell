//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The frame on the RSX (ps3_gpu.c), for both renderers.
//
//	Software renderer: the engine's 8 bit screen goes to the TV through
//	the palette, scaled by the RSX, as in 1.0-1.3.
//
//	GPU renderer (r_gpu.c): the 3D view is drawn first, into the view
//	window's place on the TV; then the window of the 8 bit screen is
//	filled with a key color, the engine draws its 2D over it (HUD,
//	messages, menus) and the screen goes up with the key pixels inside
//	the window left out.
//
//	The key is a palette index whose color some other index repeats
//	exactly (Doom's 247 is black, as 0): if the 2D ever draws it inside
//	the window, the 3D shows through one pixel, nothing else.
//

#include <stdlib.h>
#include <string.h>

#include "crispy.h"
#include "d_loop.h"
#include "i_ps3gpu.h"
#include "i_video.h"
#include "m_config.h"

int crispy_gpu_renderer = 1;
int crispy_gpu_texfilter = 0;

static boolean frame_open;
static boolean view_drawn;
static boolean last_view_drawn;     // the frame on the TV has a 3D view
static int view_x, view_y, view_w, view_h;
static int key = 247;
static boolean key_chosen;

extern void PS3_Log(const char *fmt, ...);


boolean I_GPU_Ready(void)
{
    return GPU_Ready() != 0;
}

static int Gcd(int a, int b)
{
    while (b)
    {
        int t = a % b;

        a = b;
        b = t;
    }

    return a;
}

// The engine's screen: SCREENWIDTH x SCREENHEIGHT, shown 4:3 (or wider)
// when its lines stand for 1.2 pixels (aspect ratio correction).
static void SetupScreen(void)
{
    int aw = SCREENWIDTH, ah = SCREENHEIGHT_4_3 > 0 ? SCREENHEIGHT_4_3 : SCREENHEIGHT;
    int g = Gcd(aw, ah);

    GPU_SetScreen(SCREENWIDTH, SCREENHEIGHT, aw / g, ah / g);
#ifdef PS3_BUILD
    GPU_SetFit(ps3_screen_fit);
#else
    GPU_SetFit(100);
#endif
    GPU_SetSmooth(crispy_gpu_texfilter);
}

static void OpenFrame(void)
{
    if (!frame_open)
    {
        SetupScreen();
        GPU_BeginFrame();
        frame_open = true;
    }
}

void I_GPU_BeginView(int x, int y, int w, int h)
{
    OpenFrame();
    view_x = x;
    view_y = y;
    view_w = w;
    view_h = h;
    GPU_Begin3D(x, y, w, h);
}

void I_GPU_EndView(void)
{
    int y;

    GPU_End3D();
    view_drawn = true;

    for (y = view_y; y < view_y + view_h && y < SCREENHEIGHT; y++)
    {
        memset(I_VideoBuffer + y * SCREENWIDTH + view_x, key, view_w);
    }
}

void I_GPU_FixWipeScreen(byte *screen)
{
    int y, x;

    if (!last_view_drawn)
    {
        return;
    }

    for (y = view_y; y < view_y + view_h && y < SCREENHEIGHT; y++)
    {
        byte *row = screen + y * SCREENWIDTH;

        for (x = view_x; x < view_x + view_w && x < SCREENWIDTH; x++)
        {
            if (row[x] == key)
            {
                row[x] = 0;
            }
        }
    }
}

#ifdef PS3_GPU_HOST
// The host harness: screenshots at given game tics, then quit.
extern void GPU_HostSnapshot(const char *path);

static void HostShots(void)
{
    static int next = 0;
    static int shots[64], nshots = -1, quit_tic = -1;
    const char *env, *out;
    char path[512];

    if (nshots < 0)
    {
        nshots = 0;
        env = getenv("CRISPY_HOST_SHOTS");

        while (env && *env && nshots < 64)
        {
            shots[nshots++] = atoi(env);
            env = strchr(env, ',');
            if (env)
                env++;
        }

        env = getenv("CRISPY_HOST_QUIT");
        quit_tic = env ? atoi(env) : -1;
    }

    out = getenv("CRISPY_HOST_OUT");

    while (next < nshots && gametic >= shots[next])
    {
        snprintf(path, sizeof(path), "%s/shot-%05d.ppm", out ? out : ".", shots[next]);
        GPU_HostSnapshot(path);
        next++;
    }

    if (quit_tic >= 0 && gametic >= quit_tic)
    {
        exit(0);
    }
}
#endif

void I_GPU_Present(const byte *screen)
{
    OpenFrame();
    GPU_DrawScreen(screen, view_drawn ? key : -1, view_x, view_y, view_w, view_h);
    GPU_EndFrame();
    frame_open = false;
    last_view_drawn = view_drawn;
    view_drawn = false;

#ifdef PS3_GPU_HOST
    HostShots();
#endif
}

void I_GPU_Present32(const uint32_t *argb, int w, int h, int aw, int ah)
{
    int g = Gcd(aw, ah);

    GPU_SetScreen(w, h, aw / g, ah / g);
#ifdef PS3_BUILD
    GPU_SetFit(ps3_screen_fit);
#endif
    GPU_BeginFrame();
    GPU_DrawScreen32(argb);
    GPU_EndFrame();
    frame_open = false;
    last_view_drawn = false;
    view_drawn = false;
}

void I_GPU_SetPalette(const byte *rgb)
{
    if (!key_chosen)
    {
        int i, j, best = -1;

        // an index whose color another one repeats; 247 if it is one
        key_chosen = true;

        for (i = 255; i > 0; i--)
        {
            for (j = 0; j < 256; j++)
            {
                if (j != i && !memcmp(rgb + i * 3, rgb + j * 3, 3))
                    break;
            }

            if (j < 256)
            {
                if (i == 247)
                {
                    best = 247;
                    break;
                }

                if (best < 0)
                    best = i;
            }
        }

        key = best >= 0 ? best : 247;
        PS3_Log("[gpu] key color index %d", key);
    }

    GPU_SetPalette(rgb);
}

void I_GPU_SetColormaps(const byte *maps, int rows)
{
    GPU_SetColormaps(maps, rows);
}
