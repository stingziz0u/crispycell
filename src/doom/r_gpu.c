//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The GPU renderer, Doom's part: the hooks of r_gpu_impl.h,
//	each the software renderer's rule for that detail.
//

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "m_bbox.h"
#include "p_local.h"
#include "p_mapformat.h"
#include "r_bmaps.h"
#include "r_local.h"
#include "r_sky.h"
#include "r_swirl.h"
#include "v_trans.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "crispy.h"
#include "r_gpu.h"

#ifdef CRISPY_GPU

#include "i_ps3gpu.h"

#define FIX2F_G(x)	((float)(x) * (1.0f / 65536.0f))

void R_DrawPlayerSprites(void);		// r_things.c
void R_SetupFrame(player_t *player);	// r_main.c
void R_MaybeInterpolateSector(sector_t *sector);	// r_bsp.c

static void G_InterpolateSector(sector_t *sector)
{
    R_MaybeInterpolateSector(sector);
}

// [crispy] SMMU swirling liquids
static const byte *G_SwirlFlat(int lumpnum)
{
    return (const byte *)R_DistortedFlat(lumpnum);
}

static boolean G_IsSkyFlat(int picnum)
{
    return picnum == skyflatnum;
}

static const byte *G_TextureBrightmap(int texnum)
{
    return (crispy->brightmaps & BRIGHTMAPS_TEXTURES) ? texturebrightmap[texnum] : NULL;
}

static const byte *G_FlatBrightmap(int flat)
{
    return (crispy->brightmaps & BRIGHTMAPS_TEXTURES) ? R_BrightmapForFlatNum(flat) : NULL;
}

static void G_FlatScroll(const sector_t *sec, boolean ceiling, float *du, float *dv)
{
    (void)sec;
    (void)ceiling;
    *du = *dv = 0.0f;
}

// [crispy] smoother fake contrast
static int G_FakeContrast(const seg_t *seg)
{
    return seg->fakecontrast;
}

static void G_SubsectorExtra(subsector_t *sub)
{
    (void)sub;
}

// COLORMAP: 32 light levels, invulnerability, black
static int G_ColormapRows(void)
{
    return NUMCOLORMAPS + 2;
}

static void G_MirrorProjection(float *p)
{
    // [crispy] mirrored levels
    if (crispy->fliplevels)
    {
        p[0] = -p[0];
        p[2] = -p[2];
    }
}

// [crispy] fix sprite offsets for mirrored sprites
static float G_SpriteLeft(int lump, boolean flip)
{
    return flip ? FIX2F_G(spritewidth[lump] - spriteoffset[lump]) : FIX2F_G(spriteoffset[lump]);
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

// As R_DrawVisSprite picks the column function: the translation, how it
// is see-through, and the colormap row (from the vissprite's colormap).
// lit: the sprite's bright texels (brightmaps) stay full bright.
static int G_SpriteStyle(vissprite_t *vis, const byte **translation, const byte **brightmap,
                         int *blend, uint32_t *color, boolean *lit)
{
    if (!vis->colormap[0])
    {
        // NULL colormap = shadow draw: a dark see-through shape
        *blend = GPU_BLEND_ALPHA;
        *color = GPU_RGBA(0, 0, 0, 128);
        return 0;
    }

    if (vis->mobjflags & MF_TRANSLATION)
    {
        *translation = translationtables - 256 +
            ((vis->mobjflags & MF_TRANSLATION) >> (MF_TRANSSHIFT - 8));
    }
    // [crispy] color-translated sprites (i.e. blood)
    else if (vis->translation)
    {
        *translation = vis->translation;
    }
    // [crispy] translucent sprites
    else if (crispy->translucency && vis->mobjflags & MF_TRANSLUCENT)
    {
        if (!(vis->mobjflags & (MF_NOGRAVITY | MF_COUNTITEM)) ||
            (vis->mobjflags & MF_NOGRAVITY && crispy->translucency & TRANSLUCENCY_MISSILE) ||
            (vis->mobjflags & MF_COUNTITEM && crispy->translucency & TRANSLUCENCY_ITEM))
        {
            *blend = GPU_BLEND_ALPHA;
            *color = GPU_RGBA(255, 255, 255, 168);
        }
    }

    // [crispy] brightmaps: the second colormap is the bright texels'
    *lit = vis->brightmap && vis->colormap[1] == colormaps && vis->colormap[0] != colormaps;
    *brightmap = *lit ? vis->brightmap : NULL;

    return (int)((vis->colormap[0] - colormaps) / 256);
}

static void G_DrawSky(void);

#include "r_gpu_impl.h"

// The sky, as R_DrawPlanes: always full bright (not affected by
// invulnerability either).
static void G_DrawSky(void)
{
    float texh = FIX2F_G(textureheight[skytexture]);
    float texmid = FIX2F_G(skytexturemid);
    float iscale = 320.0f / (2.0f * proj);      // pspriteiscale, per pixel

    // [crispy] stretch short skies
    if (crispy->stretchsky && texh < 200.0f)
    {
        iscale = iscale * texh / SKYSTRETCH_HEIGHT;
        texmid = texmid * texh / SKYSTRETCH_HEIGHT;
    }

    DrawSkyLayer(skytexture, texmid, iscale, 0.0f, TEX_OPAQUE, 0);
}

#endif // CRISPY_GPU
