//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The GPU renderer, Hexen's part: the hooks of r_gpu_impl.h,
//	each the software renderer's rule for that detail.
//

#include "h2def.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "m_bbox.h"
#include "p_local.h"
#include "p_mapformat.h"
#include "r_bmaps.h"
#include "r_local.h"
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
extern boolean LevelUseFullBright;	// r_things.c

static void AddSegFront(seg_t *seg, sector_t *front);	// r_gpu_impl.h

static void G_InterpolateSector(sector_t *sector)
{
    R_CheckInterpolateSector(sector);
}

// no swirling flats in Hexen
static const byte *G_SwirlFlat(int lumpnum)
{
    (void)lumpnum;
    return NULL;
}

static boolean G_IsSkyFlat(int picnum)
{
    return picnum == skyflatnum;
}

// [crispy] brightmaps: textures only, and not in fog
static const byte *G_TextureBrightmap(int texnum)
{
    return (crispy->brightmaps & BRIGHTMAPS_TEXTURES) && LevelUseFullBright ?
           texturebrightmap[texnum] : NULL;
}

static const byte *G_FlatBrightmap(int flat)
{
    (void)flat;
    return NULL;
}

// no fake contrast in Hexen
static int G_FakeContrast(const seg_t *seg)
{
    (void)seg;
    return 0;
}

// Scrolling floors (sector specials 201-224), as R_DrawPlanes does them:
// whole texels from the tic (by moving where the flat is read from),
// [crispy]'s smooth offsets in between.
static void G_FlatScroll(const sector_t *sec, boolean ceiling, float *du, float *dv)
{
    static int interpfactor;
    const int scroll = leveltime >> 1 & 63;
    int special = sec->special, n;
    fixed_t xoff = 0, yoff = 0;
    int xshift = 0, yshift = 0;

    *du = *dv = 0.0f;

    // the ceiling's visplane has no special
    if (ceiling || special < 201 || special > 224)
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

    n = (special - 201) % 3;

    switch ((special - 201) / 3)
    {
        case 0:                 // Scroll_North_xxx
            yoff = interpfactor << n;
            yshift = scroll << n & 63;
            break;
        case 1:                 // Scroll_East_xxx
            xoff = -(interpfactor << n);
            xshift = (63 - scroll) << n & 63;
            break;
        case 2:                 // Scroll_South_xxx
            yoff = -(interpfactor << n);
            yshift = (63 - scroll) << n & 63;
            break;
        case 3:                 // Scroll_West_xxx
            xoff = interpfactor << n;
            xshift = scroll << n & 63;
            break;
        case 4:                 // Scroll_NorthWest_xxx
            xoff = interpfactor << n;
            yoff = interpfactor << n;
            xshift = scroll << n & 63;
            yshift = scroll << n & 63;
            break;
        case 5:                 // Scroll_NorthEast_xxx
            xoff = -(interpfactor << n);
            yoff = interpfactor << n;
            xshift = (63 - scroll) << n & 63;
            yshift = scroll << n & 63;
            break;
        case 6:                 // Scroll_SouthEast_xxx
            xoff = -(interpfactor << n);
            yoff = -(interpfactor << n);
            xshift = (63 - scroll) << n & 63;
            yshift = (63 - scroll) << n & 63;
            break;
        case 7:                 // Scroll_SouthWest_xxx
            xoff = interpfactor << n;
            yoff = -(interpfactor << n);
            xshift = scroll << n & 63;
            yshift = (63 - scroll) << n & 63;
            break;
    }

    *du = FIX2F_G(xoff) + (float)xshift;
    *dv = FIX2F_G(yoff) + (float)yshift;
}

// polyobjects: their segs first, as R_Subsector
// a sky texture's rows: R_DrawPlanes reads its patch whole, which can be
// taller than the texture (Heretic's and Hexen's 200 row skies)
static int G_SkyHeight(int texnum)
{
    int h = textureheight[texnum] >> FRACBITS, p = R_GetPatchHeight(texnum, 0);

    return p > h ? p : h;
}

static void G_SubsectorExtra(subsector_t *sub)
{
    if (sub->poly)
    {
        int i;

        // drawn with the sector they stand in, as R_Subsector does
        for (i = 0; i < sub->poly->numsegs; i++)
        {
            AddSegFront(sub->poly->segs[i], sub->sector);
        }
    }
}

// The lines of the level's polyobjects: moved at the level's start from
// where they were drawn, they still name that sector
static boolean G_IsPolyobjLine(const line_t *line)
{
    static byte *marks;
    static const line_t *marks_for;
    static int marks_count;

    if (marks_for != lines || marks_count != numlines)
    {
        int i, j;

        free(marks);
        marks = calloc(numlines ? numlines : 1, 1);
        marks_for = lines;
        marks_count = numlines;

        for (i = 0; i < po_NumPolyobjs; i++)
        {
            for (j = 0; j < polyobjs[i].numsegs; j++)
            {
                const seg_t *seg = polyobjs[i].segs[j];

                if (seg->linedef)
                {
                    marks[seg->linedef - lines] = 1;
                }
            }
        }
    }

    return marks[line - lines] != 0;
}

// COLORMAP or the map's fade table (fog), read into colormaps
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
    return vis->psprite ? 0.0f : FIX2F_G(vis->floorclip);
}

// R_DrawVisSprite keeps the weapon still while looking up and down
static float G_PSpriteTopAdjust(const vissprite_t *vis)
{
    return vis->psprite ? (float)(viewheight / 2 - centery) : 0.0f;
}

// As R_DrawVisSprite picks the column function: the translation (by
// player class), how it is see-through (TINTTAB, crispy's 38% / 56%),
// and the colormap row. lit: the sprite's bright texels stay full bright.
static int G_SpriteStyle(vissprite_t *vis, const byte **translation, const byte **brightmap,
                         int *blend, uint32_t *color, boolean *lit)
{
    if (vis->mobjflags & MF_TRANSLATION)
    {
        *translation = translationtables - 256 + vis->class * ((maxplayers - 1) * 256) +
            ((vis->mobjflags & MF_TRANSLATION) >> (MF_TRANSSHIFT - 8));
    }

    if (vis->mobjflags & (MF_SHADOW | MF_ALTSHADOW))
    {
        *blend = GPU_BLEND_ALPHA;

        if (!(vis->mobjflags & MF_TRANSLATION) && !(vis->mobjflags & MF_SHADOW))
            *color = GPU_RGBA(255, 255, 255, 142);      // the alt shadow
        else
            *color = GPU_RGBA(255, 255, 255, 96);
    }
    // [crispy] translucent sprites
    else if (!(vis->mobjflags & MF_TRANSLATION) &&
             crispy->translucency && vis->mobjflags & MF_TRANSLUCENT &&
             ((crispy->translucency & TRANSLUCENCY_MISSILE) ||
              (vis->psprite && crispy->translucency & TRANSLUCENCY_ITEM)))
    {
        *blend = GPU_BLEND_ALPHA;
        *color = GPU_RGBA(255, 255, 255, 96);
    }

    // [crispy] brightmaps: the second colormap is the bright texels'
    *lit = vis->brightmap && vis->colormap[1] == colormaps && vis->colormap[0] != colormaps;
    *brightmap = *lit ? vis->brightmap : NULL;

    return (int)((vis->colormap[0] - colormaps) / 256);
}

static void G_DrawSky(void);

#include "r_gpu_impl.h"

// The sky, as R_DrawPlanes: one texel per row of a 320x200 screen, the
// columns moved by the map's sky scrolling; with a double sky, sky 1 in
// front and sky 2 through its color 0.
static void G_DrawSky(void)
{
    extern int Sky1Texture, Sky2Texture;
    extern fixed_t Sky1ColumnOffset, Sky2ColumnOffset;
    extern boolean DoubleSky;
    const float iscale = 1.0f / (float)(1 << crispy->hires);
    // [crispy] smooth sky scrolling when uncapped, whole columns if not
    float off1 = crispy->uncapped ? FIX2F_G(Sky1ColumnOffset) : (float)(Sky1ColumnOffset >> 16);
    float off2 = crispy->uncapped ? FIX2F_G(Sky2ColumnOffset) : (float)(Sky2ColumnOffset >> 16);

    if (DoubleSky)
    {
        DrawSkyLayer(texturetranslation[Sky2Texture], 200.0f, iscale, off2, TEX_OPAQUE, 0);
        DrawSkyLayer(texturetranslation[Sky1Texture], 200.0f, iscale, off1, TEX_KEY0, 0);
    }
    else
    {
        DrawSkyLayer(texturetranslation[Sky1Texture], 200.0f, iscale, off1, TEX_OPAQUE, 0);
    }
}

#endif // CRISPY_GPU
