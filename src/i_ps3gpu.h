//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The frame on the RSX, for both renderers (i_ps3gpu.c).
//

#ifndef __I_PS3GPU__
#define __I_PS3GPU__

#include "doomtype.h"
#include "ps3_gpu.h"

// Options > Crispness: Renderer (1 = GPU, 0 = software) and Texture
// Filter. Saved as ps3_renderer / ps3_texture_filter.
extern int crispy_gpu_renderer;
extern int crispy_gpu_texfilter;

boolean I_GPU_Ready(void);

// The GPU renderer's view window (screen pixels): opens the frame on the
// RSX, the 3D goes in between, then EndView fills the window of the
// engine's screen with the key color so the 2D drawn later goes over it.
void I_GPU_BeginView(int x, int y, int w, int h);
void I_GPU_EndView(void);

// The engine's 8 bit screen (SCREENWIDTH x SCREENHEIGHT) to the TV, over
// the 3D view if one was drawn this frame. Flips.
void I_GPU_Present(const byte *screen);

// A 32 bit picture (launcher): w x h shown at aw:ah. Flips.
void I_GPU_Present32(const uint32_t *argb, int w, int h, int aw, int ah);

// palette already through the gamma
void I_GPU_SetPalette(const byte *rgb);
void I_GPU_SetColormaps(const byte *maps, int rows);

// The screen just captured by a wipe had the 3D view in it as the key
// color: paint those pixels black so the melt doesn't show garbage.
void I_GPU_FixWipeScreen(byte *screen);

#endif
