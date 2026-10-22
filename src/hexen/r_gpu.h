//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The GPU renderer (r_gpu.c, r_gpu_impl.h).
//

#ifndef __R_GPU__
#define __R_GPU__

#include "h2def.h"
#include "r_local.h"

#ifdef CRISPY_GPU

// Options > Renderer is GPU and the RSX is up
boolean R_GPU_Active(void);

// this frame goes to the software renderer anyway
extern boolean r_gpu_softframe;

void R_GPU_RenderPlayerView(player_t *player);

// a level was loaded (P_SetupLevel)
void R_GPU_NewLevel(void);

// the textures are made again as they are drawn
void R_GPU_FlushTextures(void);

// r_things.c: while set, R_DrawVisSprite gives its vissprite to
// R_GPU_CaptureSprite instead of drawing it (the weapon)
extern boolean r_gpu_capture;
void R_GPU_CaptureSprite(vissprite_t *vis);

// r_data.c: a wall texture as palette indexes and coverage, w x h row by
// row, composed from its patches with the software renderer's rules
int R_GPU_TextureWidth(int texnum);
void R_GPU_ComposeTexture(int texnum, byte *index, byte *mask);

#endif

#endif
