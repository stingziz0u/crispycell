//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The GPU renderer, Heretic's part: the hooks of r_gpu_impl.h,
//	each the software renderer's rule for that detail.
//

#include "doomdef.h"
#include "d_loop.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "m_bbox.h"
#include "p_local.h"
#include "p_mapformat.h"
#include "r_bmaps.h"
#include "r_local.h"
#include "r_swirl.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "crispy.h"
#include "r_gpu.h"

#ifdef CRISPY_GPU

#include "i_ps3gpu.h"

#define FIX2F_G(x)	((float)(x) * (1.0f / 65536.0f))

void R_DrawPlayerSprites(void);		// r_things.c
extern fixed_t skyiscale;		// r_plane.c
extern int skytexturemid;		// r_plane.c
void R_CheckInterpolateSector(sector_t *sector);	// r_bsp.c

static void G_InterpolateSector(sector_t *sector)
{
    R_CheckInterpolateSector(sector);
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

// Scrolling floors (sector specials 4, 20-39), as R_DrawPlanes does them:
// the offsets of [crispy]'s smooth scrolling, interpolated between tics.
#define FLATSCROLL_GPU(X) \
    ((interpfactor << (X)) - (((63 - ((leveltime >> 1) & 63)) << (X) & 63) * FRACUNIT))

static void G_FlatScroll(const sector_t *sec, boolean ceiling, float *du, float *dv)
{
    static int interpfactor;
    fixed_t xoff = 0, yoff = 0;
    int special = sec->special;

    *du = *dv = 0.0f;

    if (ceiling)
    {
        return;
    }

    // [crispy] old value if uncapped and paused: scrolling stops smoothly
    if (crispy->uncapped && leveltime > oldleveltime)
    {
        // scrolling advances every *other* gametic
        if (leveltime & 1)
            interpfactor = (FRACUNIT + fractionaltic) >> 1;
        else
            interpfactor = fractionaltic >> 1;
    }
    else if (!crispy->uncapped)
    {
        interpfactor = 0;
    }

    if (special >= 25 && special <= 29)         // Scroll_North
        yoff = FLATSCROLL_GPU(special - 25);
    else if (special >= 20 && special <= 24)    // Scroll_East
        xoff = -FLATSCROLL_GPU(special - 20);
    else if (special >= 30 && special <= 34)    // Scroll_South
        yoff = -FLATSCROLL_GPU(special - 30);
    else if (special >= 35 && special <= 39)    // Scroll_West
        xoff = FLATSCROLL_GPU(special - 35);
    else if (special == 4)                      // Scroll_EastLavaDamage
        xoff = -FLATSCROLL_GPU(3);

    *du = FIX2F_G(xoff);
    *dv = FIX2F_G(yoff);
}

static void G_SubsectorExtra(subsector_t *sub)
{
    (void)sub;
}

static int G_ColormapRows(void)
{
    int rows = W_LumpLength(W_GetNumForName("COLORMAP")) / 256;

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

// feet in water, lava and sludge
static float G_SpriteFootclip(const vissprite_t *vis)
{
    return vis->psprite ? 0.0f : (float)vis->footclip;
}

// R_DrawVisSprite keeps the weapon still while looking up and down
static float G_PSpriteTopAdjust(const vissprite_t *vis)
{
    return vis->psprite ? (float)(viewheight / 2 - centery) : 0.0f;
}

// As R_DrawVisSprite picks the column function: the translation, how it
// is see-through, and the colormap row (from the vissprite's colormap).
// lit: the sprite's bright texels (brightmaps) stay full bright.
static int G_SpriteStyle(vissprite_t *vis, const byte **translation, const byte **brightmap,
                         int *blend, uint32_t *color, boolean *lit)
{
    if (vis->mobjflags & MF_TRANSLATION)
    {
        *translation = translationtables - 256 +
            ((vis->mobjflags & MF_TRANSLATION) >> (MF_TRANSSHIFT - 8));
    }

    if (vis->mobjflags & MF_SHADOW)
    {
        // the shadow column: through TINTTAB
        *blend = GPU_BLEND_ALPHA;
        *color = GPU_RGBA(255, 255, 255, 112);
    }
    // [crispy] translucent sprites
    else if (!(vis->mobjflags & MF_TRANSLATION) &&
             crispy->translucency && vis->mobjflags & MF_TRANSLUCENT &&
             ((crispy->translucency & TRANSLUCENCY_MISSILE) ||
              (vis->psprite && crispy->translucency & TRANSLUCENCY_ITEM)))
    {
        *blend = GPU_BLEND_ALPHA;
        *color = GPU_RGBA(255, 255, 255, 112);
    }

    if (!vis->colormap[0])
    {
        return 0;
    }

    // [crispy] brightmaps: the second colormap is the bright texels'
    *lit = vis->brightmap && vis->colormap[1] == colormaps && vis->colormap[0] != colormaps;
    *brightmap = *lit ? vis->brightmap : NULL;

    return (int)((vis->colormap[0] - colormaps) / 256);
}

static void G_DrawSky(void);

#include "r_gpu_impl.h"

// The sky, as R_DrawPlanes: always full bright, one texel per row of a
// 320x200 screen whatever the view size.
static void G_DrawSky(void)
{
    DrawSkyLayer(skytexture, FIX2F_G(skytexturemid), FIX2F_G(skyiscale), 0.0f, false, 0);
}

#endif // CRISPY_GPU
