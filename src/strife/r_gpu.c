//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The GPU renderer, Strife's part: the hooks of r_gpu_impl.h,
//	each the software renderer's rule for that detail.
//

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "deh_str.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "m_bbox.h"
#include "p_local.h"
#include "p_mapformat.h"
#include "r_local.h"
#include "r_sky.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "crispy.h"
#include "r_gpu.h"

#ifdef CRISPY_GPU

#include "i_ps3gpu.h"

#define FIX2F_G(x)	((float)(x) * (1.0f / 65536.0f))

void R_DrawPlayerSprites(void);		// r_things.c
void R_CheckInterpolateSector(sector_t *sector);	// r_bsp.c

static void G_InterpolateSector(sector_t *sector)
{
    R_CheckInterpolateSector(sector);
}

// no swirling flats in Strife
static const byte *G_SwirlFlat(int lumpnum)
{
    (void)lumpnum;
    return NULL;
}

static boolean G_IsSkyFlat(int picnum)
{
    return picnum == skyflatnum;
}

// no brightmaps in Crispy Strife
static const byte *G_TextureBrightmap(int texnum)
{
    (void)texnum;
    return NULL;
}

static const byte *G_FlatBrightmap(int flat)
{
    (void)flat;
    return NULL;
}

// [crispy] smoother fake contrast
static int G_FakeContrast(const seg_t *seg)
{
    return seg->fakecontrast;
}

static void G_FlatScroll(const sector_t *sec, boolean ceiling, float *du, float *dv)
{
    (void)sec;
    (void)ceiling;
    *du = *dv = 0.0f;
}

static void G_SubsectorExtra(subsector_t *sub)
{
    (void)sub;
}

// no polyobjects here
static boolean G_IsPolyobjLine(const line_t *line)
{
    (void)line;
    return false;
}

// COLORMAP: 32 light levels, then the inverse one (Sigil) and more
static int G_ColormapRows(void)
{
    int rows = W_LumpLength(W_GetNumForName(DEH_String("COLORMAP"))) / 256;

    return rows > 64 ? 64 : rows;
}

static void G_MirrorProjection(float *p)
{
    (void)p;
}

static float G_SpriteLeft(int lump, boolean flip)
{
    (void)flip;
    return FIX2F_G(spriteoffset[lump]);
}

static float G_SpriteFootclip(const vissprite_t *vis)
{
    (void)vis;
    return 0.0f;
}

// R_DrawPSprite already moved the weapon for free look
static float G_PSpriteTopAdjust(const vissprite_t *vis)
{
    (void)vis;
    return 0.0f;
}

// As R_DrawVisSprite picks the column function: the translation, and the
// two see-through ways of XLATAB (crispy's 75% / 25%).
static int G_SpriteStyle(vissprite_t *vis, const byte **translation, const byte **brightmap,
                         int *blend, uint32_t *color, boolean *lit)
{
    const int trans = vis->mobjflags & MF_TRANSLATION;

    *brightmap = NULL;
    *lit = false;

    if (trans)
    {
        *translation = translationtables - 256 + (trans >> (MF_TRANSSHIFT - 8));
    }

    if (vis->mobjflags & MF_SHADOW)
    {
        *blend = GPU_BLEND_ALPHA;

        if (!trans && vis->mobjflags & MF_MVIS)
            *color = GPU_RGBA(255, 255, 255, 192);
        else
            *color = GPU_RGBA(255, 255, 255, 64);
    }
    else if (vis->mobjflags & MF_MVIS)
    {
        // [STRIFE] only MF_MVIS: not drawn at all
        *blend = GPU_BLEND_ALPHA;
        *color = GPU_RGBA(255, 255, 255, 0);
    }
    // [crispy] translucent sprites
    else if (!trans && crispy->translucency && vis->mobjflags2 & MF_TRANSLUCENT &&
             crispy->translucency & TRANSLUCENCY_MISSILE)
    {
        *blend = GPU_BLEND_ALPHA;
        *color = GPU_RGBA(255, 255, 255, 192);
    }

    return (int)((vis->colormap - colormaps) / 256);
}

static void G_DrawSky(void);

#include "r_gpu_impl.h"

// The sky, as R_DrawPlanes: always full bright (not affected by the
// inverse colormap either).
static void G_DrawSky(void)
{
    float iscale = 320.0f / (2.0f * proj);      // pspriteiscale, per pixel

    DrawSkyLayer(skytexture, FIX2F_G(skytexturemid), iscale, 0.0f, TEX_OPAQUE, 0);
}

#endif // CRISPY_GPU
