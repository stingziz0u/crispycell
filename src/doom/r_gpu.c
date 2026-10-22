//
// Copyright(C) 2026 the CrispyCell contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//	[PS3] The GPU renderer: the 3D view drawn by the RSX (ps3_gpu.c)
//	instead of column by column on the CPU.
//
//	It reads the same level data the software renderer reads and
//	keeps its rules: texture pegging, light levels through the
//	COLORMAP (by distance, with fake contrast and gun flashes), sky
//	mapped by view angle, sprite rotations and translations, the
//	player's weapon. What changes is where pixels come from: walls,
//	flats and sprites are polygons, so the view is 720p and the depth
//	buffer replaces the clipping arrays.
//
//	Visibility: the BSP is walked front to back with an angle clipper
//	(like R_RenderBSPNode with solidsegs, but in fine angle buckets
//	instead of screen columns): one-sided walls and closed doors hide
//	what is behind them. Flats need polygons, which the nodes don't
//	have: they are carved at level load by cutting a big square with
//	each node's partition line down to every subsector, and then with
//	its segs.
//
//	The 2D (status bar, HUD, menus) is still drawn by the engine into
//	its 8 bit screen; the view window is filled with a key color and
//	i_ps3gpu.c lets the 3D picture show through it.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "i_ps3gpu.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "m_bbox.h"
#include "p_local.h"
#include "p_mapformat.h"
#include "r_bmaps.h"
#include "r_local.h"
#include "r_sky.h"
#include "v_trans.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "crispy.h"
#include "r_gpu.h"

#ifdef CRISPY_GPU

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

#define FIX2F(x)	((float)(x) * (1.0f / 65536.0f))
#define DISTMAP_GPU	2

extern int numrpsprites;	// r_things.c
extern int numtextures;		// r_data.c
void R_SetupFrame(player_t *player);	// r_main.c
void R_MaybeInterpolateSector(sector_t *sector);	// r_bsp.c
extern void PS3_Log(const char *fmt, ...);

#define BASEYCENTER_GPU	100	// r_things.c BASEYCENTER
#define BAM2RAD		(3.14159265358979323846 / 2147483648.0)

typedef struct
{
    gpu_vert_t *v;
    int n, max;
} vbuf_t;

static void VB_Need(vbuf_t *b, int more)
{
    if (b->n + more > b->max)
    {
        b->max = (b->n + more) * 2 + 1024;
        b->v = I_Realloc(b->v, b->max * sizeof(gpu_vert_t));
    }
}

static gpu_vert_t *VB_Add(vbuf_t *b, int n)
{
    gpu_vert_t *v;

    VB_Need(b, n);
    v = b->v + b->n;
    b->n += n;
    return v;
}

// the darkest light row: COLORMAP's rows after it are invulnerability
// and black, only for fixed use
static float maxrow = 31.0f;

static void SetVert(gpu_vert_t *v, float x, float y, float z, float u, float t,
                    float light, uint32_t color)
{
    v->x = x;
    v->y = y;
    v->z = z;
    v->w = 1.0f;
    v->u = u;
    v->v = t;
    v->light = light;
    v->maxrow = maxrow;
    v->color = color;
}

// Doom map coordinates (x east, y north, z up) go to the vertex as is:
// the matrix does the view transform.

// ---------------------------------------------------------------------------
// Texture cache
// ---------------------------------------------------------------------------

static gpu_tex_t **walltex;     // [numtextures * 2]: opaque, masked
static int walltex_count;
static gpu_tex_t **flattex;     // [numflats]
static int flattex_count;

#define SPRITE_HASH 1024

typedef struct spritetex_s
{
    int lump;                   // sprite lump index (not + firstspritelump)
    const byte *translation;    // NULL or the color translation
    const byte *brightmap;
    gpu_tex_t *tex;
    struct spritetex_s *next;
} spritetex_t;

static spritetex_t *spritehash[SPRITE_HASH];

static byte *scratch_index, *scratch_mask, *scratch_bright;
static int scratch_size;

static void Scratch(int pixels)
{
    if (pixels > scratch_size)
    {
        scratch_size = pixels;
        scratch_index = I_Realloc(scratch_index, pixels);
        scratch_mask = I_Realloc(scratch_mask, pixels);
        scratch_bright = I_Realloc(scratch_bright, pixels);
    }
}

static void FlushTextures(void)
{
    int i;

    for (i = 0; i < walltex_count * 2; i++)
    {
        GPU_TexFree(walltex[i]);
        walltex[i] = NULL;
    }

    for (i = 0; i < flattex_count; i++)
    {
        GPU_TexFree(flattex[i]);
        flattex[i] = NULL;
    }

    for (i = 0; i < SPRITE_HASH; i++)
    {
        while (spritehash[i])
        {
            spritetex_t *n = spritehash[i]->next;

            GPU_TexFree(spritehash[i]->tex);
            free(spritehash[i]);
            spritehash[i] = n;
        }
    }
}

// Before a frame (never during one: its surfaces point to the textures).
// Out of texture memory: drop everything, it comes back as it is drawn.
static void CheckTextureCache(void)
{
    if (walltex_count != numtextures || flattex_count != numflats)
    {
        FlushTextures();
        free(walltex);
        free(flattex);
        walltex = calloc(numtextures * 2, sizeof(*walltex));
        walltex_count = numtextures;
        flattex = calloc(numflats, sizeof(*flattex));
        flattex_count = numflats;
    }

    if (GPU_TexMemUsed() > GPU_TexMemTotal() / 10 * 8)
    {
        PS3_Log("[gpu] texture memory almost full (%u KB), flushing the cache",
                GPU_TexMemUsed() >> 10);
        FlushTextures();
    }
}

static void Brightness(byte *bright, const byte *index, int n, const byte *brightmap)
{
    int i;

    if (!brightmap || !(crispy->brightmaps))
    {
        memset(bright, 0, n);
        return;
    }

    for (i = 0; i < n; i++)
    {
        bright[i] = brightmap[index[i]] ? 1 : 0;
    }
}

// Walls are opaque: where no patch covers a texture, the software
// renderer draws index 0 (usually black). Only 2s mid textures have holes.
static gpu_tex_t *WallTextureEx(int texnum, boolean masked)
{
    gpu_tex_t **slot;
    int w, h;

    if (texnum <= 0 || texnum >= numtextures)
    {
        return NULL;
    }

    slot = &walltex[texnum * 2 + (masked ? 1 : 0)];

    if (!*slot)
    {
        w = R_GPU_TextureWidth(texnum);
        h = textureheight[texnum] >> FRACBITS;
        Scratch(w * h);
        R_GPU_ComposeTexture(texnum, scratch_index, scratch_mask);
        Brightness(scratch_bright, scratch_index, w * h,
                   (crispy->brightmaps & BRIGHTMAPS_TEXTURES) ? texturebrightmap[texnum] : NULL);
        *slot = GPU_TexCreate(w, h, scratch_index, masked ? scratch_mask : NULL, scratch_bright);
    }

    return *slot;
}

static gpu_tex_t *WallTexture(int texnum)
{
    return WallTextureEx(texnum, false);
}

static gpu_tex_t *FlatTexture(int picnum)
{
    int flat;
    const byte *data;

    flat = flattranslation[picnum];

    // [crispy] SMMU swirling flats: drawn still
    if (flat == -1)
    {
        flat = picnum;
    }

    if (flat < 0 || flat >= numflats)
    {
        return NULL;
    }

    if (!flattex[flat])
    {
        data = W_CacheLumpNum(firstflat + flat, PU_CACHE);
        Scratch(64 * 64);
        memcpy(scratch_index, data, 64 * 64);
        Brightness(scratch_bright, scratch_index, 64 * 64,
                   (crispy->brightmaps & BRIGHTMAPS_TEXTURES) ? R_BrightmapForFlatNum(flat) : NULL);
        flattex[flat] = GPU_TexCreate(64, 64, scratch_index, NULL, scratch_bright);
    }

    return flattex[flat];
}

// A patch (sprite, weapon) as a texture, through a color translation.
static gpu_tex_t *PatchTexture(int lumpnum, const byte *translation, const byte *brightmap,
                               int *width, int *height)
{
    patch_t *patch = W_CacheLumpNum(lumpnum, PU_CACHE);
    int w = SHORT(patch->width), h = SHORT(patch->height);
    int x;

    *width = w;
    *height = h;

    Scratch(w * h);
    memset(scratch_index, 0, w * h);
    memset(scratch_mask, 0, w * h);

    for (x = 0; x < w; x++)
    {
        const column_t *col = (const column_t *)((const byte *)patch + LONG(patch->columnofs[x]));
        int top = -1;

        while (col->topdelta != 0xff)
        {
            const byte *src = (const byte *)col + 3;
            int y, count = col->length;

            // [crispy] DeePsea tall patches
            if (col->topdelta <= top)
                top += col->topdelta;
            else
                top = col->topdelta;

            for (y = 0; y < count; y++)
            {
                int row = top + y;

                if (row >= 0 && row < h)
                {
                    byte p = src[y];

                    scratch_index[row * w + x] = translation ? translation[p] : p;
                    scratch_mask[row * w + x] = 1;
                }
            }

            col = (const column_t *)((const byte *)col + col->length + 4);
        }
    }

    Brightness(scratch_bright, scratch_index, w * h, brightmap);

    return GPU_TexCreate(w, h, scratch_index, scratch_mask, scratch_bright);
}

static gpu_tex_t *SpriteTexture(int lump, const byte *translation, const byte *brightmap)
{
    unsigned key = ((unsigned)lump * 31u + (unsigned)(uintptr_t)translation) % SPRITE_HASH;
    spritetex_t *s;
    int w, h;

    for (s = spritehash[key]; s; s = s->next)
    {
        if (s->lump == lump && s->translation == translation && s->brightmap == brightmap)
        {
            return s->tex;
        }
    }

    s = calloc(1, sizeof(*s));
    s->lump = lump;
    s->translation = translation;
    s->brightmap = brightmap;
    s->tex = PatchTexture(lump + firstspritelump, translation, brightmap, &w, &h);
    s->next = spritehash[key];
    spritehash[key] = s;

    return s->tex;
}

// ---------------------------------------------------------------------------
// Subsector polygons, carved at level load
// ---------------------------------------------------------------------------

typedef struct
{
    double x, y;
} cpoint_t;

static float *sspoly_xy;        // all the points, x y x y...
static int *sspoly_first;       // [numsubsectors]
static int *sspoly_count;       // [numsubsectors]
static int sspoly_used, sspoly_max;
static subsector_t *sspoly_level;   // what the polygons were made for

#define CARVE_MAX 256

// keeps the part of poly on the side where cross <= 0 (keep_right) or >= 0
static int ClipPoly(const cpoint_t *in, int n, cpoint_t *out,
                    double lx, double ly, double ldx, double ldy, int keep_right)
{
    int i, m = 0;
    const double eps = 1.0 / 256.0;

    for (i = 0; i < n && m < CARVE_MAX - 1; i++)
    {
        const cpoint_t *a = &in[i], *b = &in[(i + 1) % n];
        double da = ldx * (a->y - ly) - ldy * (a->x - lx);
        double db = ldx * (b->y - ly) - ldy * (b->x - lx);
        double len = sqrt(ldx * ldx + ldy * ldy);
        int ina, inb;

        if (len > 0)
        {
            da /= len;
            db /= len;
        }

        if (!keep_right)
        {
            da = -da;
            db = -db;
        }

        ina = da <= eps;
        inb = db <= eps;

        if (ina)
        {
            out[m++] = *a;
        }

        if (ina != inb && fabs(da - db) > 1e-12)
        {
            double t = da / (da - db);

            out[m].x = a->x + (b->x - a->x) * t;
            out[m].y = a->y + (b->y - a->y) * t;
            m++;
        }
    }

    return m;
}

static void StorePoly(int ss, const cpoint_t *p, int n)
{
    int i;

    if (sspoly_used + n * 2 > sspoly_max)
    {
        sspoly_max = (sspoly_used + n * 2) * 2 + 4096;
        sspoly_xy = I_Realloc(sspoly_xy, sspoly_max * sizeof(float));
    }

    sspoly_first[ss] = sspoly_used / 2;
    sspoly_count[ss] = n;

    for (i = 0; i < n; i++)
    {
        sspoly_xy[sspoly_used++] = (float)p[i].x;
        sspoly_xy[sspoly_used++] = (float)p[i].y;
    }
}

static void CarveSubsector(int ss, const cpoint_t *poly, int n)
{
    static cpoint_t a[CARVE_MAX], b[CARVE_MAX];
    const subsector_t *sub = &subsectors[ss];
    int i;

    memcpy(a, poly, n * sizeof(cpoint_t));

    for (i = 0; i < sub->numlines && n >= 3; i++)
    {
        const seg_t *seg = &segs[sub->firstline + i];
        double x1 = FIX2F(seg->v1->r_x), y1 = FIX2F(seg->v1->r_y);
        double x2 = FIX2F(seg->v2->r_x), y2 = FIX2F(seg->v2->r_y);

        if (x1 == x2 && y1 == y2)
        {
            continue;
        }

        n = ClipPoly(a, n, b, x1, y1, x2 - x1, y2 - y1, 1);
        memcpy(a, b, n * sizeof(cpoint_t));
    }

    if (n >= 3)
    {
        StorePoly(ss, a, n);
    }
    else
    {
        sspoly_count[ss] = 0;
    }
}

static void CarveNode(int nodenum, const cpoint_t *poly, int n, int depth)
{
    cpoint_t front[CARVE_MAX], back[CARVE_MAX];
    const node_t *node;
    int nf, nb;

    if (nodenum & NF_SUBSECTOR)
    {
        int ss = nodenum & ~NF_SUBSECTOR;

        if (ss < numsubsectors)
        {
            CarveSubsector(ss, poly, n);
        }

        return;
    }

    if (depth > 512 || nodenum >= numnodes)
    {
        return;
    }

    node = &nodes[nodenum];

    // children[0] is the front: R_PointOnSide == 0, the right side
    nf = ClipPoly(poly, n, front, FIX2F(node->x), FIX2F(node->y),
                  FIX2F(node->dx), FIX2F(node->dy), 1);
    nb = ClipPoly(poly, n, back, FIX2F(node->x), FIX2F(node->y),
                  FIX2F(node->dx), FIX2F(node->dy), 0);

    CarveNode(node->children[0], front, nf, depth + 1);
    CarveNode(node->children[1], back, nb, depth + 1);
}

static void BuildSubsectorPolys(void)
{
    cpoint_t square[4];
    fixed_t bbox[4];
    int i;

    free(sspoly_first);
    free(sspoly_count);
    sspoly_first = calloc(numsubsectors, sizeof(int));
    sspoly_count = calloc(numsubsectors, sizeof(int));
    sspoly_used = 0;

    M_ClearBox(bbox);

    for (i = 0; i < numvertexes; i++)
    {
        M_AddToBox(bbox, vertexes[i].x, vertexes[i].y);
    }

    square[0].x = FIX2F(bbox[BOXLEFT]) - 64;
    square[0].y = FIX2F(bbox[BOXTOP]) + 64;
    square[1].x = FIX2F(bbox[BOXRIGHT]) + 64;
    square[1].y = FIX2F(bbox[BOXTOP]) + 64;
    square[2].x = FIX2F(bbox[BOXRIGHT]) + 64;
    square[2].y = FIX2F(bbox[BOXBOTTOM]) - 64;
    square[3].x = FIX2F(bbox[BOXLEFT]) - 64;
    square[3].y = FIX2F(bbox[BOXBOTTOM]) - 64;

    if (numnodes == 0)
    {
        CarveSubsector(0, square, 4);
    }
    else
    {
        CarveNode(numnodes - 1, square, 4, 0);
    }

    sspoly_level = subsectors;
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// the view, in floats
static float vx, vy, vz, vcos, vsin;
static float proj;              // projection, in screen pixels
static float cx, cy;            // view center, in view window pixels
static float vw, vh;            // view window size, screen pixels
static float mvp[16];

// light
static int fixedrow;            // -1: none
static float wallcap;
static int extralight_gpu;

// the sky
static int sky_texture;
static boolean sky_seen;

// draw lists
typedef enum
{
    KIND_WALL,          // walls, light by distance
    KIND_FLAT,          // floors and ceilings
    KIND_MASKED,        // 2s mid textures
    KIND_SKY,           // sky surfaces (depth and stencil only)
    NUM_KINDS
} kind_t;

typedef struct
{
    gpu_tex_t *tex;
    int kind;
    int first, count;           // in surfverts
} surf_t;

static vbuf_t surfverts;
static surf_t *surfs;
static int numsurfs, maxsurfs;

typedef struct
{
    gpu_tex_t *tex;
    float depth;
    float light;
    int fixed;                  // colormap row or -1
    int blend;
    uint32_t color;
    gpu_vert_t v[6];
} sprite_t;

static sprite_t *sprlist;
static int numsprites_gpu, maxsprites_gpu;

static void AddSurf(gpu_tex_t *tex, int kind, const gpu_vert_t *v, int n)
{
    surf_t *s;

    if (!tex && kind != KIND_SKY)
    {
        return;
    }

    if (numsurfs == maxsurfs)
    {
        maxsurfs = maxsurfs ? maxsurfs * 2 : 4096;
        surfs = I_Realloc(surfs, maxsurfs * sizeof(*surfs));
    }

    if (kind == KIND_SKY)
    {
        sky_seen = true;
    }

    s = &surfs[numsurfs++];
    s->tex = tex;
    s->kind = kind;
    s->first = surfverts.n;
    s->count = n;
    memcpy(VB_Add(&surfverts, n), v, n * sizeof(gpu_vert_t));
}

// Doom's light: the colormap row a surface starts from, before distance
static float StartMap(int lightlevel, int contrast)
{
    int lightnum = (lightlevel >> LIGHTSEGSHIFT) + extralight_gpu * LIGHTBRIGHT + contrast;

    if (lightnum < 0)
        lightnum = 0;
    else if (lightnum >= LIGHTLEVELS)
        lightnum = LIGHTLEVELS - 1;

    return (float)(((LIGHTLEVELS - LIGHTBRIGHT - lightnum) * 2) * NUMCOLORMAPS / LIGHTLEVELS);
}

static fixed_t FloorHeight(const sector_t *s)
{
    return s->interpfloorheight;
}

static fixed_t CeilingHeight(const sector_t *s)
{
    return s->interpceilingheight;
}

// ---------------------------------------------------------------------------
// Angle clipper
// ---------------------------------------------------------------------------

#define CLIP_BUCKETS 2048

static byte clipsolid[CLIP_BUCKETS];
static angle_t gclipangle;      // half the field of view, plus a margin

// angle relative to the view (left positive, as Doom) -> bucket
static int AngleToBucket(angle_t a, int roundup)
{
    double f = ((double)(int)(gclipangle - a)) / (2.0 * (double)gclipangle) * CLIP_BUCKETS;
    int b = roundup ? (int)ceil(f) : (int)floor(f);

    if (b < 0)
        b = 0;
    if (b > CLIP_BUCKETS)
        b = CLIP_BUCKETS;
    return b;
}

// Clips [angle1 (left), angle2 (right)] to the view. 0 = not in view.
static boolean ClipToView(angle_t *angle1, angle_t *angle2)
{
    angle_t span = *angle1 - *angle2, tspan;

    if (span >= ANG180)
    {
        return false;
    }

    tspan = *angle1 + gclipangle;

    if (tspan > 2 * gclipangle)
    {
        tspan -= 2 * gclipangle;

        if (tspan >= span)
            return false;

        *angle1 = gclipangle;
    }

    tspan = gclipangle - *angle2;

    if (tspan > 2 * gclipangle)
    {
        tspan -= 2 * gclipangle;

        if (tspan >= span)
            return false;

        *angle2 = -gclipangle;
    }

    return true;
}

static boolean RangeVisible(angle_t a1, angle_t a2)
{
    int b1 = AngleToBucket(a1, 0), b2 = AngleToBucket(a2, 1), b;

    if (b2 <= b1)
    {
        b2 = b1 + 1;
    }

    if (b2 > CLIP_BUCKETS)
    {
        b2 = CLIP_BUCKETS;
        if (b1 >= b2)
            b1 = b2 - 1;
    }

    for (b = b1; b < b2; b++)
    {
        if (!clipsolid[b])
            return true;
    }

    return false;
}

static void RangeSolid(angle_t a1, angle_t a2)
{
    // only buckets completely covered
    int b1 = AngleToBucket(a1, 1), b2 = AngleToBucket(a2, 0);

    if (b2 > b1)
    {
        memset(clipsolid + b1, 1, b2 - b1);
    }
}

static const int checkcoord_gpu[12][4] =
{
    {3, 0, 2, 1},
    {3, 0, 2, 0},
    {3, 1, 2, 0},
    {0},
    {2, 0, 2, 1},
    {0, 0, 0, 0},
    {3, 1, 3, 0},
    {0},
    {2, 0, 3, 1},
    {2, 1, 3, 1},
    {2, 1, 3, 0}
};

static boolean CheckBBox(const fixed_t *bspcoord)
{
    int boxx, boxy, boxpos;
    angle_t angle1, angle2;

    if (viewx <= bspcoord[BOXLEFT])
        boxx = 0;
    else if (viewx < bspcoord[BOXRIGHT])
        boxx = 1;
    else
        boxx = 2;

    if (viewy >= bspcoord[BOXTOP])
        boxy = 0;
    else if (viewy > bspcoord[BOXBOTTOM])
        boxy = 1;
    else
        boxy = 2;

    boxpos = (boxy << 2) + boxx;

    if (boxpos == 5)
    {
        return true;
    }

    angle1 = R_PointToAngleCrispy(bspcoord[checkcoord_gpu[boxpos][0]],
                                  bspcoord[checkcoord_gpu[boxpos][1]]) - viewangle;
    angle2 = R_PointToAngleCrispy(bspcoord[checkcoord_gpu[boxpos][2]],
                                  bspcoord[checkcoord_gpu[boxpos][3]]) - viewangle;

    if (angle1 - angle2 >= ANG180)
    {
        return true;
    }

    if (!ClipToView(&angle1, &angle2))
    {
        return false;
    }

    return RangeVisible(angle1, angle2);
}

// ---------------------------------------------------------------------------
// Walls
// ---------------------------------------------------------------------------

// A quad of a wall between two heights, texture top at 'textop' (world z
// of the texture's first row), u0/u1 in texels at v1/v2.
static void WallQuad(gpu_tex_t *tex, int kind, float x1, float y1, float x2, float y2,
                     float zbot, float ztop, float u0, float u1, float textop,
                     float light)
{
    gpu_vert_t v[6];
    float tw, th, s0, s1, t0, t1;


    if (ztop <= zbot)
    {
        return;
    }

    if (tex)
    {
        tw = (float)GPU_TexWidth(tex);
        th = (float)GPU_TexHeight(tex);
    }
    else
    {
        tw = th = 1.0f;
    }

    s0 = u0 / tw;
    s1 = u1 / tw;
    t0 = (textop - ztop) / th;
    t1 = (textop - zbot) / th;

    SetVert(&v[0], x1, y1, ztop, s0, t0, light, 0xffffffffu);
    SetVert(&v[1], x2, y2, ztop, s1, t0, light, 0xffffffffu);
    SetVert(&v[2], x2, y2, zbot, s1, t1, light, 0xffffffffu);
    v[3] = v[0];
    v[4] = v[2];
    SetVert(&v[5], x1, y1, zbot, s0, t1, light, 0xffffffffu);

    AddSurf(tex, kind, v, 6);
}

static boolean IsSky(int picnum)
{
    return picnum == skyflatnum;
}

static void AddSeg(seg_t *seg)
{
    sector_t *front = seg->frontsector, *back = seg->backsector;
    side_t *side = seg->sidedef;
    line_t *line = seg->linedef;
    angle_t angle1, angle2, a1, a2;
    float x1, y1, x2, y2, len, u0, u1, light;
    float ffloor, fceil, bfloor, bceil;
    boolean solid;

    if (!line || !side || !front)
    {
        return;
    }

    angle1 = R_PointToAngleCrispy(seg->v1->r_x, seg->v1->r_y);
    angle2 = R_PointToAngleCrispy(seg->v2->r_x, seg->v2->r_y);

    // back side: not drawn
    if (angle1 - angle2 >= ANG180)
    {
        return;
    }

    a1 = angle1 - viewangle;
    a2 = angle2 - viewangle;

    if (!ClipToView(&a1, &a2))
    {
        return;
    }

    ffloor = FIX2F(FloorHeight(front));
    fceil = FIX2F(CeilingHeight(front));

    if (back)
    {
        bfloor = FIX2F(FloorHeight(back));
        bceil = FIX2F(CeilingHeight(back));

        // [crispy] a door closed because its back sector is shut
        solid = (CeilingHeight(back) <= FloorHeight(back) &&
                 (CeilingHeight(back) >= CeilingHeight(front) || side->toptexture) &&
                 (FloorHeight(back) <= FloorHeight(front) || side->bottomtexture) &&
                 (back->ceilingpic != skyflatnum || front->ceilingpic != skyflatnum)) ||
                CeilingHeight(back) <= FloorHeight(front) ||
                FloorHeight(back) >= CeilingHeight(front);
    }
    else
    {
        bfloor = bceil = 0.0f;
        solid = true;
    }

    if (!RangeVisible(a1, a2))
    {
        return;
    }

    // the automap shows what has been seen
    line->flags |= ML_MAPPED;

    if (solid)
    {
        RangeSolid(a1, a2);
    }

    x1 = FIX2F(seg->v1->r_x);
    y1 = FIX2F(seg->v1->r_y);
    x2 = FIX2F(seg->v2->r_x);
    y2 = FIX2F(seg->v2->r_y);
    len = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    u0 = FIX2F(seg->offset + side->textureoffset);
    u1 = u0 + len;

    light = fixedrow >= 0 ? 0.0f : StartMap(front->rlightlevel, seg->fakecontrast);

    if (!back)
    {
        int texnum = texturetranslation[side->midtexture];
        float textop;

        if (line->flags & ML_DONTPEGBOTTOM)
            textop = ffloor + FIX2F(textureheight[side->midtexture]);
        else
            textop = fceil;

        textop += FIX2F(side->rowoffset);
        WallQuad(WallTexture(texnum), KIND_WALL, x1, y1, x2, y2, ffloor, fceil,
                 u0, u1, textop, light);

        // above a sky ceiling, the sky hides everything behind
        if (IsSky(front->ceilingpic))
        {
            WallQuad(NULL, KIND_SKY, x1, y1, x2, y2, fceil, 32768.0f, 0, 0, 0, 0);
        }

        return;
    }

    // upper wall
    {
        float top = fceil;

        // hack to allow height changes in outdoor areas: no upper wall
        // between two skies, the sky shows there
        if (IsSky(front->ceilingpic) && IsSky(back->ceilingpic))
        {
            WallQuad(NULL, KIND_SKY, x1, y1, x2, y2, bceil, 32768.0f, 0, 0, 0, 0);
        }
        else
        {
            if (bceil < top)
            {
                int texnum = texturetranslation[side->toptexture];
                float textop;

                if (line->flags & ML_DONTPEGTOP)
                    textop = fceil;
                else
                    textop = bceil + FIX2F(textureheight[side->toptexture]);

                textop += FIX2F(side->rowoffset);
                WallQuad(WallTexture(texnum), KIND_WALL, x1, y1, x2, y2,
                         bceil > ffloor ? bceil : ffloor, top, u0, u1, textop, light);
            }

            if (IsSky(front->ceilingpic))
            {
                WallQuad(NULL, KIND_SKY, x1, y1, x2, y2, fceil, 32768.0f, 0, 0, 0, 0);
            }
        }
    }

    // lower wall
    if (bfloor > ffloor)
    {
        int texnum = texturetranslation[side->bottomtexture];
        float textop;

        if (line->flags & ML_DONTPEGBOTTOM)
            textop = fceil;
        else
            textop = bfloor;

        textop += FIX2F(side->rowoffset);
        WallQuad(WallTexture(texnum), KIND_WALL, x1, y1, x2, y2, ffloor,
                 bfloor < fceil ? bfloor : fceil, u0, u1, textop, light);
    }

    // masked middle texture: once, inside the opening
    if (side->midtexture)
    {
        int texnum = texturetranslation[side->midtexture];
        gpu_tex_t *tex = WallTextureEx(texnum, true);
        float textop, texbot, lo, hi;

        if (tex)
        {
            if (line->flags & ML_DONTPEGBOTTOM)
                textop = (ffloor > bfloor ? ffloor : bfloor) + FIX2F(textureheight[texnum]);
            else
                textop = fceil < bceil ? fceil : bceil;

            textop += FIX2F(side->rowoffset);
            texbot = textop - FIX2F(textureheight[texnum]);
            lo = ffloor > bfloor ? ffloor : bfloor;
            hi = fceil < bceil ? fceil : bceil;

            if (texbot > lo)
                lo = texbot;
            if (textop < hi)
                hi = textop;

            WallQuad(tex, KIND_MASKED, x1, y1, x2, y2, lo, hi, u0, u1, textop, light);
        }
    }
}

// ---------------------------------------------------------------------------
// Subsectors, flats
// ---------------------------------------------------------------------------

static void AddFlat(int ss, sector_t *sec, boolean ceiling)
{
    gpu_vert_t v[3 * (CARVE_MAX - 2)];
    const float *xy = sspoly_xy + sspoly_first[ss] * 2;
    int n = sspoly_count[ss], i, k = 0;
    int picnum = ceiling ? sec->ceilingpic : sec->floorpic;
    float z = FIX2F(ceiling ? CeilingHeight(sec) : FloorHeight(sec));
    float light = 0.0f;
    gpu_tex_t *tex = NULL;
    int kind = KIND_FLAT;

    if (n < 3)
    {
        return;
    }

    // a plane on the wrong side of the view isn't seen
    if (ceiling ? (z <= vz && !IsSky(picnum)) : (z >= vz))
    {
        return;
    }

    if (IsSky(picnum))
    {
        kind = KIND_SKY;
    }
    else
    {
        tex = FlatTexture(picnum);
        light = fixedrow >= 0 ? 0.0f : StartMap(sec->rlightlevel, 0);
    }

    for (i = 1; i + 1 < n; i++)
    {
        int idx[3] = { 0, i, i + 1 }, j;

        for (j = 0; j < 3; j++)
        {
            float px = xy[idx[j] * 2], py = xy[idx[j] * 2 + 1];

            // flats are aligned to the world: 64 units per tile
            SetVert(&v[k++], px, py, z, px / 64.0f, -py / 64.0f, light, 0xffffffffu);
        }
    }

    AddSurf(tex, kind, v, k);
}

static int *visible_sectors;
static int numvisible_sectors, maxvisible_sectors;

static void AddSubsector(int num)
{
    subsector_t *sub = &subsectors[num];
    sector_t *sec = sub->sector;
    int i;

    if (sec->validcount != validcount)
    {
        sec->validcount = validcount;

        if (numvisible_sectors == maxvisible_sectors)
        {
            maxvisible_sectors = maxvisible_sectors ? maxvisible_sectors * 2 : 256;
            visible_sectors = I_Realloc(visible_sectors, maxvisible_sectors * sizeof(int));
        }

        visible_sectors[numvisible_sectors++] = sec - sectors;
    }

    if (sspoly_count && sspoly_count[num] >= 3)
    {
        AddFlat(num, sec, false);
        AddFlat(num, sec, true);
    }

    for (i = 0; i < sub->numlines; i++)
    {
        AddSeg(&segs[sub->firstline + i]);
    }
}

static void RenderBSPNode(int bspnum)
{
    while (!(bspnum & NF_SUBSECTOR))
    {
        const node_t *bsp = &nodes[bspnum];
        int side = R_PointOnSide(viewx, viewy, (node_t *)bsp);

        // the front side first, then the back if it can be seen
        RenderBSPNode(bsp->children[side]);

        if (!CheckBBox(bsp->bbox[side ^ 1]))
        {
            return;
        }

        bspnum = bsp->children[side ^ 1];
    }

    AddSubsector(bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

static sprite_t *NewSprite(void)
{
    if (numsprites_gpu == maxsprites_gpu)
    {
        maxsprites_gpu = maxsprites_gpu ? maxsprites_gpu * 2 : 256;
        sprlist = I_Realloc(sprlist, maxsprites_gpu * sizeof(*sprlist));
    }

    return &sprlist[numsprites_gpu++];
}

static void ProjectSprite(mobj_t *thing, float light)
{
    fixed_t interpx, interpy, interpz;
    angle_t interpangle;
    spritedef_t *sprdef;
    spriteframe_t *sprframe;
    int lump;
    boolean flip;
    const byte *translation = NULL;
    const byte *brightmap = NULL;
    gpu_tex_t *tex;
    sprite_t *spr;
    float tx, tz, rx, ry, left, right, top, bottom, w, h, gx, gy;
    float s0, s1;
    uint32_t color = 0xffffffffu;
    int fixed = fixedrow, blend = GPU_BLEND_NONE;

    if (crispy->uncapped && thing->interp == true && leveltime > oldleveltime)
    {
        interpx = LerpFixed(thing->oldx, thing->x);
        interpy = LerpFixed(thing->oldy, thing->y);
        interpz = LerpFixed(thing->oldz, thing->z);
        interpangle = LerpAngle(thing->oldangle, thing->angle);
    }
    else
    {
        interpx = thing->x;
        interpy = thing->y;
        interpz = thing->z;
        interpangle = thing->angle;
    }

    gx = FIX2F(interpx);
    gy = FIX2F(interpy);

    // in front of the view?
    tz = (gx - vx) * vcos + (gy - vy) * vsin;

    if (tz < 4.0f)
    {
        return;
    }

    tx = (gx - vx) * vsin - (gy - vy) * vcos;

    // too far off the side?
    if (fabsf(tx) > tz * 4.0f)
    {
        return;
    }

    if ((unsigned)thing->sprite >= (unsigned)numsprites)
    {
        return;
    }

    sprdef = &sprites[thing->sprite];

    if (!sprdef->numframes || (thing->frame & FF_FRAMEMASK) >= sprdef->numframes)
    {
        return;
    }

    sprframe = &sprdef->spriteframes[thing->frame & FF_FRAMEMASK];

    if (sprframe->rotate)
    {
        angle_t ang = R_PointToAngle(interpx, interpy);
        unsigned rot;

        if (sprframe->rotate == -1)
        {
            return;
        }
        else if (sprframe->rotate == 2)
        {
            const unsigned rot2 = (ang - interpangle + (unsigned)(ANG45 / 4) * 17);
            rot = (rot2 >> 29) + ((rot2 >> 25) & 8);
        }
        else
        {
            rot = (ang - interpangle + (unsigned)(ANG45 / 2) * 9) >> 29;
        }

        lump = sprframe->lump[rot];
        flip = (boolean)sprframe->flip[rot];
    }
    else
    {
        lump = sprframe->lump[0];
        flip = (boolean)sprframe->flip[0];
    }

    if (lump < 0 || lump >= numspritelumps)
    {
        return;
    }

    // [crispy] randomly flip corpse, blood and death animation sprites
    if (crispy->flipcorpses && (thing->flags & MF_FLIPPABLE) &&
        !(thing->flags & MF_SHOOTABLE) && (thing->health & 1))
    {
        flip = !flip;
    }

    if (thing->flags & MF_TRANSLATION)
    {
        translation = translationtables - 256 +
            ((thing->flags & MF_TRANSLATION) >> (MF_TRANSSHIFT - 8));
    }
    else if (crispy->coloredblood &&
             (thing->type == MT_BLOOD || thing->state - states == S_GIBS) && thing->target)
    {
        if (gamemission == pack_hacx)
        {
            if (thing->target->type == MT_BABY)
                translation = cr[CR_RED2GREEN];
        }
        else if (thing->target->type == MT_BRUISER || thing->target->type == MT_KNIGHT)
        {
            translation = cr[CR_RED2GREEN];
        }
        else if (thing->target->type == MT_HEAD)
        {
            translation = cr[CR_RED2BLUE];
        }
    }

    if (crispy->brightmaps & BRIGHTMAPS_SPRITES)
    {
        brightmap = R_BrightmapForSprite(thing->sprite);
    }

    tex = SpriteTexture(lump, translation, brightmap);

    if (!tex)
    {
        return;
    }

    w = FIX2F(spritewidth[lump]);
    h = (float)GPU_TexHeight(tex);

    // the shape's left edge, in view space x (right positive)
    left = tx - (flip ? w - FIX2F(spriteoffset[lump]) : FIX2F(spriteoffset[lump]));
    right = left + w;
    top = FIX2F(interpz + spritetopoffset[lump]);
    bottom = top - h;

    spr = NewSprite();
    spr->tex = tex;
    spr->depth = tz;

    if (thing->flags & MF_SHADOW)
    {
        // the "fuzz": a dark see-through shape
        color = GPU_RGBA(0, 0, 0, 128);
        blend = GPU_BLEND_ALPHA;
    }
    else if (fixedrow >= 0)
    {
        fixed = fixedrow;
    }
    else if (thing->frame & FF_FULLBRIGHT)
    {
        fixed = 0;
    }

    // [crispy] translucent sprites
    if (!(thing->flags & MF_SHADOW) && crispy->translucency && (thing->flags & MF_TRANSLUCENT) &&
        (!(thing->flags & (MF_NOGRAVITY | MF_COUNTITEM)) ||
         (thing->flags & MF_NOGRAVITY && crispy->translucency & TRANSLUCENCY_MISSILE) ||
         (thing->flags & MF_COUNTITEM && crispy->translucency & TRANSLUCENCY_ITEM)))
    {
        color = GPU_RGBA(255, 255, 255, 168);
        blend = GPU_BLEND_ALPHA;
    }

    spr->fixed = fixed;
    spr->light = light;
    spr->blend = blend;
    spr->color = color;

    // a quad facing the view plane, at the thing's position
    rx = vsin;
    ry = -vcos;
    s0 = flip ? 1.0f : 0.0f;
    s1 = flip ? 0.0f : 1.0f;

    {
        float lx = vx + rx * left + vcos * tz, ly = vy + ry * left + vsin * tz;
        float qx = vx + rx * right + vcos * tz, qy = vy + ry * right + vsin * tz;

        SetVert(&spr->v[0], lx, ly, top, s0, 0.0f, light, color);
        SetVert(&spr->v[1], qx, qy, top, s1, 0.0f, light, color);
        SetVert(&spr->v[2], qx, qy, bottom, s1, 1.0f, light, color);
        spr->v[3] = spr->v[0];
        spr->v[4] = spr->v[2];
        SetVert(&spr->v[5], lx, ly, bottom, s0, 1.0f, light, color);
    }
}

static void AddSprites(void)
{
    int i;

    for (i = 0; i < numvisible_sectors; i++)
    {
        sector_t *sec = &sectors[visible_sectors[i]];
        float light = fixedrow >= 0 ? 0.0f : StartMap(sec->rlightlevel, 0);
        mobj_t *thing;

        for (thing = sec->thinglist; thing; thing = thing->snext)
        {
            ProjectSprite(thing, light);
        }
    }
}

static int CompareSprites(const void *a, const void *b)
{
    const sprite_t *sa = a, *sb = b;

    // far ones first
    return sa->depth < sb->depth ? 1 : sa->depth > sb->depth ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static int CompareSurfs(const void *a, const void *b)
{
    const surf_t *sa = a, *sb = b;

    if (sa->kind != sb->kind)
        return sa->kind - sb->kind;
    if (sa->tex != sb->tex)
        return sa->tex < sb->tex ? -1 : 1;
    return sa->first - sb->first;
}

static vbuf_t batch;

static void DrawSurfs(void)
{
    gpu_draw_t d;
    int i = 0;

    qsort(surfs, numsurfs, sizeof(*surfs), CompareSurfs);

    while (i < numsurfs)
    {
        int j = i;

        batch.n = 0;

        while (j < numsurfs && surfs[j].kind == surfs[i].kind && surfs[j].tex == surfs[i].tex)
        {
            memcpy(VB_Add(&batch, surfs[j].count), surfverts.v + surfs[j].first,
                   surfs[j].count * sizeof(gpu_vert_t));
            j++;
        }

        memset(&d, 0, sizeof(d));
        d.tex = surfs[i].tex;
        d.prog = GPU_PROG_WORLD;
        d.depth_test = 1;
        d.depth_write = 1;
        d.color_write = 1;
        d.wrap = 1;
        d.fixed_row = (float)fixedrow;
        d.vis = 1280.0f;

        switch (surfs[i].kind)
        {
            case KIND_WALL:
            case KIND_MASKED:
                // (a masked mid texture's quad stops at the texture's
                // edges: wrapping only matters across)
                d.cap = wallcap;
                break;
            case KIND_FLAT:
                d.cap = 1000.0f;
                break;
            case KIND_SKY:
                d.prog = GPU_PROG_COLOR;
                d.color_write = 0;
                d.stencil = GPU_STENCIL_WRITE;
                break;
        }

        GPU_DrawTris(&d, batch.v, batch.n);
        i = j;
    }
}

static void SetOrtho(float w, float h)
{
    // view window pixels (y down) -> clip space
    float m[16] = {
        2.0f / w, 0, 0, -1.0f,
        0, -2.0f / h, 0, 1.0f,
        0, 0, 0, 0,
        0, 0, 0, 1.0f
    };

    GPU_SetMatrix(m);
}

// The sky, as the software renderer maps it: a column per view angle
// (1024 around), rows straight down from the view's center line.
static void DrawSky(void)
{
    gpu_tex_t *tex = WallTexture(sky_texture);
    gpu_draw_t d;
    vbuf_t *b = &batch;
    float texw, texh, texmid, iscale;
    int i;
    const int strips = 64;

    if (!tex)
    {
        return;
    }

    texw = (float)GPU_TexWidth(tex);
    texh = (float)GPU_TexHeight(tex);
    texmid = FIX2F(skytexturemid);
    iscale = 320.0f / (2.0f * proj);        // texels per screen pixel

    // [crispy] stretch short skies
    if (crispy->stretchsky && texh < 200.0f)
    {
        iscale = iscale * texh / SKYSTRETCH_HEIGHT;
        texmid = texmid * texh / SKYSTRETCH_HEIGHT;
    }

    b->n = 0;

    for (i = 0; i < strips; i++)
    {
        float xa = vw * i / strips, xb = vw * (i + 1) / strips;
        // view angle of a screen column, as xtoviewangle (left positive)
        double aa = atan2(cx - xa, proj), ab = atan2(cx - xb, proj);
        double ca = ((double)viewangle * BAM2RAD + aa) / (2.0 * 3.14159265358979323846) * 1024.0;
        double cb = ((double)viewangle * BAM2RAD + ab) / (2.0 * 3.14159265358979323846) * 1024.0;
        float ua = (float)(ca / texw), ub = (float)(cb / texw);
        float ta = (texmid + (0.0f - cy) * iscale) / texh;
        float tb = (texmid + (vh - cy) * iscale) / texh;
        gpu_vert_t *v = VB_Add(b, 6);

        // keep u small: it's wrapped anyway
        ub -= floorf(ua);
        ua -= floorf(ua);

        SetVert(&v[0], xa, 0.0f, 0.0f, ua, ta, 0.0f, 0xffffffffu);
        SetVert(&v[1], xb, 0.0f, 0.0f, ub, ta, 0.0f, 0xffffffffu);
        SetVert(&v[2], xb, vh, 0.0f, ub, tb, 0.0f, 0xffffffffu);
        v[3] = v[0];
        v[4] = v[2];
        SetVert(&v[5], xa, vh, 0.0f, ua, tb, 0.0f, 0xffffffffu);
    }

    memset(&d, 0, sizeof(d));
    d.tex = tex;
    d.prog = GPU_PROG_WORLD;
    d.color_write = 1;
    d.wrap = 1;
    d.stencil = GPU_STENCIL_TEST;
    // Sky is always drawn full bright: not affected by INVUL either
    d.fixed_row = 0.0f;

    SetOrtho(vw, vh);
    GPU_DrawTris(&d, b->v, b->n);
    GPU_SetMatrix(mvp);
}

static void DrawSprites(void)
{
    gpu_draw_t d;
    int i;

    qsort(sprlist, numsprites_gpu, sizeof(*sprlist), CompareSprites);

    // opaque ones first (any order: the depth buffer sorts them), then the
    // see-through ones far to near without writing depth
    for (i = 0; i < numsprites_gpu; i++)
    {
        sprite_t *s = &sprlist[i];

        if (s->blend != GPU_BLEND_NONE)
            continue;

        memset(&d, 0, sizeof(d));
        d.tex = s->tex;
        d.prog = GPU_PROG_WORLD;
        d.depth_test = 1;
        d.depth_write = 1;
        d.color_write = 1;
        d.fixed_row = (float)s->fixed;
        d.vis = 1280.0f;
        d.cap = wallcap;
        GPU_DrawTris(&d, s->v, 6);
    }

    for (i = 0; i < numsprites_gpu; i++)
    {
        sprite_t *s = &sprlist[i];

        if (s->blend == GPU_BLEND_NONE)
            continue;

        memset(&d, 0, sizeof(d));
        d.tex = s->tex;
        d.prog = GPU_PROG_WORLD;
        d.blend = s->blend;
        d.depth_test = 1;
        d.depth_write = 0;
        d.color_write = 1;
        d.fixed_row = (float)s->fixed;
        d.vis = 1280.0f;
        d.cap = wallcap;
        GPU_DrawTris(&d, s->v, 6);
    }
}

// ---------------------------------------------------------------------------
// The player's weapon
// ---------------------------------------------------------------------------

static void DrawPSprite(pspdef_t *psp, psprnum_t psprnum, float light)
{
    spritedef_t *sprdef;
    spriteframe_t *sprframe;
    int lump;
    boolean flip;
    gpu_tex_t *tex;
    const byte *brightmap = NULL;
    float pscale, tx, x1, x2, ytop, h, s0, s1;
    float texturemid;
    gpu_vert_t v[6];
    gpu_draw_t d;
    uint32_t color = 0xffffffffu;
    int blend = GPU_BLEND_NONE, fixed = fixedrow;

    if ((unsigned)psp->state->sprite >= (unsigned)numsprites)
        return;

    sprdef = &sprites[psp->state->sprite];

    if (!sprdef->numframes || (psp->state->frame & FF_FRAMEMASK) >= sprdef->numframes)
        return;

    sprframe = &sprdef->spriteframes[psp->state->frame & FF_FRAMEMASK];
    lump = sprframe->lump[0];
    flip = (boolean)sprframe->flip[0] ^ crispy->flipweapons;

    if (crispy->brightmaps & BRIGHTMAPS_SPRITES)
        brightmap = R_BrightmapForState(psp->state - states);

    tex = SpriteTexture(lump, NULL, brightmap);

    if (!tex)
        return;

    // in view window pixels, as R_DrawPSprite
    pscale = 2.0f * proj / ORIGWIDTH;
    tx = FIX2F(psp->sx2) - ORIGWIDTH / 2;
    tx -= flip ? 2 * tx - FIX2F(spriteoffset[lump]) + FIX2F(spritewidth[lump]) : FIX2F(spriteoffset[lump]);
    x1 = cx + tx * pscale;
    x2 = x1 + FIX2F(spritewidth[lump]) * pscale;
    // [crispy] weapons drawn 1 pixel too high when player is idle
    texturemid = BASEYCENTER_GPU + 0.5f / (1 << crispy->hires) -
                 (FIX2F(psp->sy2) - FIX2F(spritetopoffset[lump]));
    // [crispy] free look: the weapon stays put in the window
    ytop = (float)(viewheight / 2) - texturemid * pscale;
    h = (float)GPU_TexHeight(tex) * pscale;
    s0 = flip ? 1.0f : 0.0f;
    s1 = flip ? 0.0f : 1.0f;

    if (viewplayer->powers[pw_invisibility] > 4 * 32 || viewplayer->powers[pw_invisibility] & 8)
    {
        color = GPU_RGBA(0, 0, 0, 128);
        blend = GPU_BLEND_ALPHA;
    }
    else if (fixedrow >= 0)
    {
        fixed = fixedrow;
    }
    else if (psp->state->frame & FF_FULLBRIGHT)
    {
        fixed = 0;
    }
    else
    {
        // spritelights[MAXLIGHTSCALE-1]: the closest light
        int row = (int)light - (int)wallcap;

        fixed = row < 0 ? 0 : row;
    }

    // [crispy] translucent gun flash sprites
    if (psprnum == ps_flash && crispy->translucency && blend == GPU_BLEND_NONE)
    {
        color = GPU_RGBA(255, 255, 255, 168);
        blend = GPU_BLEND_ALPHA;
    }

    SetVert(&v[0], x1, ytop, 0.0f, s0, 0.0f, 0.0f, color);
    SetVert(&v[1], x2, ytop, 0.0f, s1, 0.0f, 0.0f, color);
    SetVert(&v[2], x2, ytop + h, 0.0f, s1, 1.0f, 0.0f, color);
    v[3] = v[0];
    v[4] = v[2];
    SetVert(&v[5], x1, ytop + h, 0.0f, s0, 1.0f, 0.0f, color);

    memset(&d, 0, sizeof(d));
    d.tex = tex;
    d.prog = GPU_PROG_WORLD;
    d.blend = blend;
    d.color_write = 1;
    d.fixed_row = (float)fixed;
    GPU_DrawTris(&d, v, 6);
}

static void DrawPlayerSprites(void)
{
    int i;
    pspdef_t *psp;
    float light = StartMap(viewplayer->mo->subsector->sector->rlightlevel, 0);

    SetOrtho(vw, vh);

    for (i = 0, psp = viewplayer->psprites; i < numrpsprites; i++, psp++)
    {
        if (psp->state)
        {
            DrawPSprite(psp, i, light);
        }
    }

    GPU_SetMatrix(mvp);
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

static void SetupMatrix(void)
{
    // view space: X right, Y up, Z forward (depth)
    // X = dx * sin - dy * cos, Y = z - viewz, Z = dx * cos + dy * sin
    const float n = 2.0f, f = 65536.0f;
    float a = (f + n) / (f - n), bb = -2.0f * f * n / (f - n);
    float p[16], v[16];
    int r, c, k;

    memset(p, 0, sizeof(p));
    memset(v, 0, sizeof(v));

    // projection: pixels of the view window, as the software renderer
    p[0] = 2.0f * proj / vw;
    p[2] = 2.0f * cx / vw - 1.0f;
    p[5] = 2.0f * proj / vh;
    p[6] = 1.0f - 2.0f * cy / vh;
    p[10] = a;
    p[11] = bb;
    p[14] = 1.0f;

    // [crispy] mirrored levels
    if (crispy->fliplevels)
    {
        p[0] = -p[0];
        p[2] = -p[2];
    }

    v[0] = vsin;
    v[1] = -vcos;
    v[3] = -(vx * vsin - vy * vcos);
    v[6] = 1.0f;
    v[7] = -vz;
    v[8] = vcos;
    v[9] = vsin;
    v[11] = -(vx * vcos + vy * vsin);
    v[15] = 1.0f;

    for (r = 0; r < 4; r++)
    {
        for (c = 0; c < 4; c++)
        {
            float s = 0.0f;

            for (k = 0; k < 4; k++)
                s += p[r * 4 + k] * v[k * 4 + c];

            mvp[r * 4 + c] = s;
        }
    }
}

boolean r_gpu_softframe;

boolean R_GPU_Active(void)
{
    return crispy_gpu_renderer && !r_gpu_softframe && I_GPU_Ready();
}

void R_GPU_NewLevel(void)
{
    // made on the first frame drawn by the GPU renderer; the textures
    // stay, most are shared between levels
    sspoly_level = NULL;
}

void R_GPU_RenderPlayerView(player_t *player)
{
    double halffov;

    R_SetupFrame(player);

    // [crispy] smooth texture scrolling
    R_InterpolateTextureOffsets();

    if (sspoly_level != subsectors)
    {
        BuildSubsectorPolys();
    }

    CheckTextureCache();

    // [AM] the sectors' interpolated heights (the software renderer does it
    // sector by sector as its BSP walk reaches them)
    {
        int i;

        for (i = 0; i < numsectors; i++)
            R_MaybeInterpolateSector(&sectors[i]);
    }

    vx = FIX2F(viewx);
    vy = FIX2F(viewy);
    vz = FIX2F(viewz);
    vcos = (float)cos((double)viewangle * BAM2RAD);
    vsin = (float)sin((double)viewangle * BAM2RAD);

    // the view window, in screen pixels; detail level doesn't matter here
    vw = (float)scaledviewwidth;
    vh = (float)viewheight;
    proj = FIX2F(projection) * (1 << detailshift);
    cx = vw / 2.0f;
    cy = (float)centery;

    halffov = atan2(vw / 2.0, proj) + 3.0 * 3.14159265358979323846 / 180.0;
    gclipangle = (angle_t)(halffov / BAM2RAD);

    // the colormaps through the palette, for the fragment programs
    {
        static const lighttable_t *uploaded;

        if (colormaps != uploaded)
        {
            I_GPU_SetColormaps(colormaps, NUMCOLORMAPS + 2);
            uploaded = colormaps;
        }
    }

    fixedrow = fixedcolormap ? (int)((fixedcolormap - colormaps) / 256) : -1;
    maxrow = (float)(NUMCOLORMAPS - 1);
    extralight_gpu = extralight;
    wallcap = (float)((47 * NONWIDEWIDTH / (int)(2.0f * proj)) / DISTMAP_GPU);
    sky_texture = skytexture;
    sky_seen = false;

    memset(clipsolid, 0, sizeof(clipsolid));
    numsurfs = 0;
    surfverts.n = 0;
    numsprites_gpu = 0;
    numvisible_sectors = 0;

    if (numnodes)
        RenderBSPNode(numnodes - 1);
    else
        AddSubsector(0);

    AddSprites();

    // to the RSX
    I_GPU_BeginView(viewwindowx, viewwindowy, scaledviewwidth, viewheight);
    SetupMatrix();
    GPU_SetMatrix(mvp);

    DrawSurfs();

    if (sky_seen)
    {
        DrawSky();
    }

    DrawSprites();
    DrawPlayerSprites();

    I_GPU_EndView();
}

#endif // CRISPY_GPU
