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
//	instead of column by column on the CPU. The same code for the four
//	engines: each game's r_gpu.c includes its own headers, defines the
//	hooks below (G_*) and includes this file.
//
//	It reads the same level data the software renderer reads and keeps
//	its rules: texture pegging, light levels through the COLORMAP (by
//	distance, with fake contrast and gun flashes), sky mapped by view
//	angle. Sprites and the player's weapon go through the engine's own
//	R_AddSprites / R_DrawPlayerSprites (rotations, translations, light,
//	weapon offsets are each game's), and only the last step, drawing
//	the vissprite, is replaced. What changes is where pixels come from:
//	walls, flats and sprites are polygons, so the view is 720p and the
//	depth buffer replaces the clipping arrays.
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
//	Hooks a game defines before including this file:
//	  G_IsSkyFlat(picnum)            a sky ceiling or floor
//	  G_FlatScroll(sec, ceiling, &du, &dv)  scrolling flats (texels)
//	  G_TextureBrightmap(texnum), G_FlatBrightmap(flat)  NULL = none
//	  G_SpriteLeft(lump, flip)       left edge offset of a sprite (units)
//	  G_SpriteStyle(vis, ...)        translation, see-through, colormap row
//	  G_SpriteFootclip(vis)          units cut from the bottom (water)
//	  G_PSpriteTopAdjust()           screen rows added to a weapon's top
//	  G_SubsectorExtra(sub)          more segs of a subsector (polyobjs)
//	  G_ColormapRows()               rows in the COLORMAP to upload
//	  G_DrawSky()                    the sky, with DrawSkyLayer
//	  G_MirrorProjection(p)          [crispy] mirrored levels
//	  G_InterpolateSector(sec)       [AM] the sector's interpolated heights
//	  G_SwirlFlat(lump)              [crispy] the swirling flat now, or NULL
//	  G_FakeContrast(seg)            light added to a wall by its direction
//

#ifdef CRISPY_GPU

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "i_ps3gpu.h"

#define FIX2F(x)	((float)(x) * (1.0f / 65536.0f))
#define BAM2RAD		(3.14159265358979323846 / 2147483648.0)
#define DISTMAP_GPU	2

extern int numtextures;		// r_data.c
extern void PS3_Log(const char *fmt, ...);

// [PS3] r_things.c: R_DrawVisSprite hands its vissprite here while set
boolean r_gpu_capture;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

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

// Map coordinates (x east, y north, z up) go to the vertex as is: the
// matrix does the view transform.
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

// ---------------------------------------------------------------------------
// Texture cache
// ---------------------------------------------------------------------------

// [numtextures * WALLTEX_KINDS]: opaque; masked by the patches (holes);
// masked where the index is 0 (the front layer of a double sky)
#define WALLTEX_KINDS 3
enum { TEX_OPAQUE, TEX_MASKED, TEX_KEY0 };
static gpu_tex_t **walltex;
static int walltex_count;
static gpu_tex_t **flattex;     // [numflats]
static int flattex_count;
static gpu_tex_t **swirltex;    // [numflats] SMMU swirling flats, this tic's
static int *swirltic;

#define SPRITE_HASH 1024

typedef struct spritetex_s
{
    int lump;                   // patch lump number
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

// R_GPU_FlushTextures: what the textures were made from changed
static boolean textures_stale;

static void FlushTextures(void)
{
    int i;

    for (i = 0; i < walltex_count * WALLTEX_KINDS; i++)
    {
        GPU_TexFree(walltex[i]);
        walltex[i] = NULL;
    }

    for (i = 0; i < flattex_count; i++)
    {
        GPU_TexFree(flattex[i]);
        flattex[i] = NULL;
        GPU_TexFree(swirltex[i]);
        swirltex[i] = NULL;
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
        free(swirltex);
        free(swirltic);
        walltex = calloc(numtextures * WALLTEX_KINDS, sizeof(*walltex));
        walltex_count = numtextures;
        flattex = calloc(numflats, sizeof(*flattex));
        swirltex = calloc(numflats, sizeof(*swirltex));
        swirltic = calloc(numflats, sizeof(*swirltic));
        flattex_count = numflats;
    }

    if (textures_stale)
    {
        FlushTextures();
        textures_stale = false;
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

    if (!brightmap)
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
static gpu_tex_t *WallTextureEx(int texnum, int kind)
{
    gpu_tex_t **slot;
    int w, h, i;

    if (texnum <= 0 || texnum >= numtextures)
    {
        return NULL;
    }

    slot = &walltex[texnum * WALLTEX_KINDS + kind];

    if (!*slot)
    {
        w = R_GPU_TextureWidth(texnum);
        h = textureheight[texnum] >> FRACBITS;
        Scratch(w * h);
        R_GPU_ComposeTexture(texnum, scratch_index, scratch_mask);
        Brightness(scratch_bright, scratch_index, w * h, G_TextureBrightmap(texnum));

        if (kind == TEX_KEY0)
        {
            for (i = 0; i < w * h; i++)
                scratch_mask[i] = scratch_index[i] != 0;
        }

        *slot = GPU_TexCreate(w, h, scratch_index, kind != TEX_OPAQUE ? scratch_mask : NULL,
                              scratch_bright);
    }

    return *slot;
}

static gpu_tex_t *WallTexture(int texnum)
{
    return WallTextureEx(texnum, TEX_OPAQUE);
}

static gpu_tex_t *FlatTexture(int picnum)
{
    int flat;
    const byte *data;

    if (picnum < 0 || picnum >= numflats)
    {
        return NULL;
    }

    flat = flattranslation[picnum];

    // [crispy] SMMU swirling flats: a new texture every tic
    if (flat == -1)
    {
        const byte *swirl = G_SwirlFlat(firstflat + picnum);

        if (swirl)
        {
            if (!swirltex[picnum] || swirltic[picnum] != leveltime)
            {
                GPU_TexFree(swirltex[picnum]);
                Scratch(64 * 64);
                memcpy(scratch_index, swirl, 64 * 64);
                Brightness(scratch_bright, scratch_index, 64 * 64, G_FlatBrightmap(picnum));
                swirltex[picnum] = GPU_TexCreate(64, 64, scratch_index, NULL, scratch_bright);
                swirltic[picnum] = leveltime;
            }

            return swirltex[picnum];
        }

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
        Brightness(scratch_bright, scratch_index, 64 * 64, G_FlatBrightmap(flat));
        flattex[flat] = GPU_TexCreate(64, 64, scratch_index, NULL, scratch_bright);
    }

    return flattex[flat];
}

// A patch (sprite, weapon) as a texture, through a color translation.
static gpu_tex_t *PatchTexture(int lumpnum, const byte *translation, const byte *brightmap)
{
    patch_t *patch = W_CacheLumpNum(lumpnum, PU_CACHE);
    int w = SHORT(patch->width), h = SHORT(patch->height);
    int x;

    if (w <= 0 || h <= 0)
    {
        return NULL;
    }

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

static gpu_tex_t *SpriteTexture(int lumpnum, const byte *translation, const byte *brightmap)
{
    unsigned key = ((unsigned)lumpnum * 31u + (unsigned)(uintptr_t)translation) % SPRITE_HASH;
    spritetex_t *s;

    for (s = spritehash[key]; s; s = s->next)
    {
        if (s->lump == lumpnum && s->translation == translation && s->brightmap == brightmap)
        {
            return s->tex;
        }
    }

    s = calloc(1, sizeof(*s));
    s->lump = lumpnum;
    s->translation = translation;
    s->brightmap = brightmap;
    s->tex = PatchTexture(lumpnum, translation, brightmap);
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

// keeps the part of poly on the right of the line (keep_right) or the left
static int ClipPoly(const cpoint_t *in, int n, cpoint_t *out,
                    double lx, double ly, double ldx, double ldy, int keep_right)
{
    int i, m = 0;
    const double eps = 1.0 / 256.0;
    double len = sqrt(ldx * ldx + ldy * ldy);

    for (i = 0; i < n && m < CARVE_MAX - 1; i++)
    {
        const cpoint_t *a = &in[i], *b = &in[(i + 1) % n];
        double da = ldx * (a->y - ly) - ldy * (a->x - lx);
        double db = ldx * (b->y - ly) - ldy * (b->x - lx);
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
    int fixed;                  // colormap row, or -1: the vertices' light
    int blend;
    gpu_vert_t v[6];
} sprite_t;

static sprite_t *sprlist;
static int numsprites_gpu, maxsprites_gpu;

// screen sprites (the weapon) captured from R_DrawVisSprite
static sprite_t *pspr;
static int numpspr, maxpspr;

static void AddSurf(gpu_tex_t *tex, int kind, const gpu_vert_t *v, int n)
{
    surf_t *s;

    if (!tex && kind != KIND_SKY)
    {
        return;
    }

    if (kind == KIND_SKY)
    {
        sky_seen = true;
    }

    if (numsurfs == maxsurfs)
    {
        maxsurfs = maxsurfs ? maxsurfs * 2 : 4096;
        surfs = I_Realloc(surfs, maxsurfs * sizeof(*surfs));
    }

    s = &surfs[numsurfs++];
    s->tex = tex;
    s->kind = kind;
    s->first = surfverts.n;
    s->count = n;
    memcpy(VB_Add(&surfverts, n), v, n * sizeof(gpu_vert_t));
}

// The software renderer's light: the colormap row a surface starts from,
// before distance
static float StartMap(int lightlevel, int contrast)
{
    int lightnum = (lightlevel >> LIGHTSEGSHIFT) + extralight_gpu * LIGHTBRIGHT + contrast;

    if (lightnum < 0)
        lightnum = 0;
    else if (lightnum >= LIGHTLEVELS)
        lightnum = LIGHTLEVELS - 1;

    return (float)(((LIGHTLEVELS - LIGHTBRIGHT - lightnum) * 2) * NUMCOLORMAPS / LIGHTLEVELS);
}

// ---------------------------------------------------------------------------
// Angle clipper
// ---------------------------------------------------------------------------

#define CLIP_BUCKETS 2048

static byte clipsolid[CLIP_BUCKETS];
static angle_t gclipangle;      // half the field of view, plus a margin

// angle relative to the view (left positive, as the BSP code) -> bucket
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

// Clips [angle1 (left), angle2 (right)] to the view. false = not in view.
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

static void SkyWall(float x1, float y1, float x2, float y2, float zbot)
{
    WallQuad(NULL, KIND_SKY, x1, y1, x2, y2, zbot, 32768.0f, 0, 0, 0, 0);
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

    ffloor = FIX2F(front->interpfloorheight);
    fceil = FIX2F(front->interpceilingheight);

    if (back)
    {
        bfloor = FIX2F(back->interpfloorheight);
        bceil = FIX2F(back->interpceilingheight);

        // [crispy] a door closed because its back sector is shut
        solid = (back->interpceilingheight <= back->interpfloorheight &&
                 (back->interpceilingheight >= front->interpceilingheight || side->toptexture) &&
                 (back->interpfloorheight <= front->interpfloorheight || side->bottomtexture) &&
                 (!G_IsSkyFlat(back->ceilingpic) || !G_IsSkyFlat(front->ceilingpic))) ||
                back->interpceilingheight <= front->interpfloorheight ||
                back->interpfloorheight >= front->interpceilingheight;
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

    light = fixedrow >= 0 ? 0.0f : StartMap(front->rlightlevel, G_FakeContrast(seg));

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
        if (G_IsSkyFlat(front->ceilingpic))
        {
            SkyWall(x1, y1, x2, y2, fceil);
        }

        return;
    }

    // upper wall
    if (G_IsSkyFlat(front->ceilingpic) && G_IsSkyFlat(back->ceilingpic))
    {
        // hack to allow height changes in outdoor areas: no upper wall
        // between two skies, the sky shows there
        SkyWall(x1, y1, x2, y2, bceil);
    }
    else
    {
        if (bceil < fceil)
        {
            int texnum = texturetranslation[side->toptexture];
            float textop;

            if (line->flags & ML_DONTPEGTOP)
                textop = fceil;
            else
                textop = bceil + FIX2F(textureheight[side->toptexture]);

            textop += FIX2F(side->rowoffset);
            WallQuad(WallTexture(texnum), KIND_WALL, x1, y1, x2, y2,
                     bceil > ffloor ? bceil : ffloor, fceil, u0, u1, textop, light);
        }

        if (G_IsSkyFlat(front->ceilingpic))
        {
            SkyWall(x1, y1, x2, y2, fceil);
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
        gpu_tex_t *tex = WallTextureEx(texnum, TEX_MASKED);
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
    float z = FIX2F(ceiling ? sec->interpceilingheight : sec->interpfloorheight);
    float light = 0.0f, du = 0.0f, dv = 0.0f;
    gpu_tex_t *tex = NULL;
    int kind = KIND_FLAT;

    if (n < 3)
    {
        return;
    }

    // a plane on the wrong side of the view isn't seen
    if (ceiling ? (z <= vz && !G_IsSkyFlat(picnum)) : (z >= vz))
    {
        return;
    }

    if (G_IsSkyFlat(picnum))
    {
        kind = KIND_SKY;
    }
    else
    {
        tex = FlatTexture(picnum);
        light = fixedrow >= 0 ? 0.0f : StartMap(sec->rlightlevel, 0);
        G_FlatScroll(sec, ceiling, &du, &dv);
    }

    for (i = 1; i + 1 < n; i++)
    {
        int idx[3] = { 0, i, i + 1 }, j;

        for (j = 0; j < 3; j++)
        {
            float px = xy[idx[j] * 2], py = xy[idx[j] * 2 + 1];

            // flats are aligned to the world: 64 units per tile
            SetVert(&v[k++], px, py, z, (px + du) / 64.0f, (-py + dv) / 64.0f, light, 0xffffffffu);
        }
    }

    AddSurf(tex, kind, v, k);
}

// the sectors whose things go to the sprite list, once each
static int *sector_frame;
static int sector_frame_count;
static int gpu_frame;
static int *visible_sectors;
static int numvisible_sectors, maxvisible_sectors;

static void AddSubsector(int num)
{
    subsector_t *sub = &subsectors[num];
    sector_t *sec = sub->sector;
    int i, s = sec - sectors;

    if (sector_frame[s] != gpu_frame)
    {
        sector_frame[s] = gpu_frame;

        if (numvisible_sectors == maxvisible_sectors)
        {
            maxvisible_sectors = maxvisible_sectors ? maxvisible_sectors * 2 : 256;
            visible_sectors = I_Realloc(visible_sectors, maxvisible_sectors * sizeof(int));
        }

        visible_sectors[numvisible_sectors++] = s;
    }

    if (sspoly_count && sspoly_count[num] >= 3)
    {
        AddFlat(num, sec, false);
        AddFlat(num, sec, true);
    }

    G_SubsectorExtra(sub);

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
// Sprites: the engine's vissprites, as quads
// ---------------------------------------------------------------------------

static sprite_t *NewSprite(sprite_t **list, int *num, int *max)
{
    if (*num == *max)
    {
        *max = *max ? *max * 2 : 256;
        *list = I_Realloc(*list, *max * sizeof(**list));
    }

    return &(*list)[(*num)++];
}

// A sprite's row is already the software renderer's final colormap: the
// light path must not clamp it to the light rows
static void SpriteMaxRow(sprite_t *spr)
{
    int i;

    for (i = 0; i < 6; i++)
    {
        spr->v[i].maxrow = 63.0f;
    }
}

// A world sprite: a quad facing the view plane at the thing's position,
// as big as the patch (one texel per unit).
static void WorldSprite(vissprite_t *vis)
{
    const byte *translation = NULL, *brightmap = NULL;
    int blend = GPU_BLEND_NONE, row, lumpnum;
    boolean lit = false;
    float light;
    uint32_t color = 0xffffffffu;
    gpu_tex_t *tex;
    sprite_t *spr;
    boolean flip = vis->xiscale < 0;
    float gx = FIX2F(vis->gx), gy = FIX2F(vis->gy);
    float tz, tx, left, right, top, bottom, w, h, clip, t1, s0, s1;
    float lx, ly, qx, qy;

    row = G_SpriteStyle(vis, &translation, &brightmap, &blend, &color, &lit);

    if (row < 0)
    {
        return;
    }

    // The software renderer lights a whole sprite with one colormap,
    // chosen at its center: that row, fixed. Brightmapped texels need
    // the light path of the fragment program instead (it keeps them at
    // row 0): the row goes as the vertices' light, with no distance.
    light = lit ? (float)row : 0.0f;

    lumpnum = vis->patch + firstspritelump;
    tex = SpriteTexture(lumpnum, translation, brightmap);

    if (!tex)
    {
        return;
    }

    tz = (gx - vx) * vcos + (gy - vy) * vsin;
    tx = (gx - vx) * vsin - (gy - vy) * vcos;

    if (tz < 1.0f)
    {
        return;
    }

    w = FIX2F(spritewidth[vis->patch]);
    h = (float)GPU_TexHeight(tex);
    left = tx - G_SpriteLeft(vis->patch, flip);
    right = left + w;
    top = FIX2F(vis->gzt);
    bottom = top - h;

    // feet in water and the like
    clip = G_SpriteFootclip(vis);
    t1 = 1.0f;

    if (clip > 0.0f && clip < h)
    {
        bottom += clip;
        t1 = (h - clip) / h;
    }

    spr = NewSprite(&sprlist, &numsprites_gpu, &maxsprites_gpu);
    spr->tex = tex;
    spr->depth = tz;
    spr->fixed = lit ? -1 : row;
    spr->blend = blend;

    s0 = flip ? 1.0f : 0.0f;
    s1 = flip ? 0.0f : 1.0f;
    lx = vx + vsin * left + vcos * tz;
    ly = vy - vcos * left + vsin * tz;
    qx = vx + vsin * right + vcos * tz;
    qy = vy - vcos * right + vsin * tz;

    SetVert(&spr->v[0], lx, ly, top, s0, 0.0f, light, color);
    SetVert(&spr->v[1], qx, qy, top, s1, 0.0f, light, color);
    SetVert(&spr->v[2], qx, qy, bottom, s1, t1, light, color);
    spr->v[3] = spr->v[0];
    spr->v[4] = spr->v[2];
    SetVert(&spr->v[5], lx, ly, bottom, s0, t1, light, color);
    SpriteMaxRow(spr);
}

// A screen sprite (the weapon, a crosshair): placed as R_DrawVisSprite
// would draw it, in view window pixels.
void R_GPU_CaptureSprite(vissprite_t *vis)
{
    const byte *translation = NULL, *brightmap = NULL;
    int blend = GPU_BLEND_NONE, row;
    boolean lit = false;
    float light;
    uint32_t color = 0xffffffffu;
    gpu_tex_t *tex;
    sprite_t *spr;
    float w, h, sf, xi, xa, xb, left, right, s0, s1, scale, top, detail;

    row = G_SpriteStyle(vis, &translation, &brightmap, &blend, &color, &lit);

    if (row < 0)
    {
        return;
    }

    light = lit ? (float)row : 0.0f;
    tex = SpriteTexture(vis->patch + firstspritelump, translation, brightmap);

    if (!tex || !vis->xiscale)
    {
        return;
    }

    detail = (float)(1 << detailshift);
    w = (float)GPU_TexWidth(tex);
    h = (float)GPU_TexHeight(tex);
    sf = FIX2F(vis->startfrac);
    xi = FIX2F(vis->xiscale);

    // texel 0 and texel w, in pixel columns
    xa = vis->x1 - sf / xi;
    xb = vis->x1 + (w - sf) / xi;

    if (xa < xb)
    {
        left = xa;
        right = xb;
        s0 = 0.0f;
        s1 = 1.0f;
    }
    else
    {
        left = xb;
        right = xa;
        s0 = 1.0f;
        s1 = 0.0f;
    }

    left = left * detail + 0.5f;
    right = right * detail + 0.5f;
    scale = FIX2F(vis->scale);
    top = cy - FIX2F(vis->texturemid) * scale + G_PSpriteTopAdjust(vis);

    spr = NewSprite(&pspr, &numpspr, &maxpspr);
    spr->tex = tex;
    spr->depth = 0.0f;
    spr->fixed = lit ? -1 : row;
    spr->blend = blend;

    SetVert(&spr->v[0], left, top, 0.0f, s0, 0.0f, light, color);
    SetVert(&spr->v[1], right, top, 0.0f, s1, 0.0f, light, color);
    SetVert(&spr->v[2], right, top + h * scale, 0.0f, s1, 1.0f, light, color);
    spr->v[3] = spr->v[0];
    spr->v[4] = spr->v[2];
    SetVert(&spr->v[5], left, top + h * scale, 0.0f, s0, 1.0f, light, color);
    SpriteMaxRow(spr);
}

static void CollectSprites(void)
{
    vissprite_t *vis;
    int i;

    R_ClearSprites();

    for (i = 0; i < numvisible_sectors; i++)
    {
        R_AddSprites(&sectors[visible_sectors[i]]);
    }

    for (vis = vissprites; vis < vissprite_p; vis++)
    {
        WorldSprite(vis);
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
                // zlight: the light table by distance steps
                d.cap = 1000.0f;
                d.zstep = (float)(1 << (LIGHTZSHIFT - FRACBITS));
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

// A sky layer, as the software renderer maps it: a column per view angle
// (1024 around), rows straight down from the view's center line; only
// where the sky surfaces marked the stencil.
// kind: TEX_OPAQUE, or TEX_KEY0 for a layer over another one
static void DrawSkyLayer(int texnum, float texmid, float iscale, float uoffset,
                         int kind, int fixed)
{
    gpu_tex_t *tex = WallTextureEx(texnum, kind);
    gpu_draw_t d;
    vbuf_t *b = &batch;
    float texw, texh;
    int i;
    const int strips = 64;

    if (!tex)
    {
        return;
    }

    texw = (float)GPU_TexWidth(tex);
    texh = (float)GPU_TexHeight(tex);
    b->n = 0;

    for (i = 0; i < strips; i++)
    {
        float xa = vw * i / strips, xb = vw * (i + 1) / strips;
        // view angle of a screen column, as xtoviewangle (left positive)
        double aa = atan2(cx - xa, proj), ab = atan2(cx - xb, proj);
        double ca = ((double)viewangle * BAM2RAD + aa) / (2.0 * 3.14159265358979323846) * 1024.0;
        double cb = ((double)viewangle * BAM2RAD + ab) / (2.0 * 3.14159265358979323846) * 1024.0;
        float ua = (float)((ca + uoffset) / texw), ub = (float)((cb + uoffset) / texw);
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
    d.fixed_row = (float)fixed;

    SetOrtho(vw, vh);
    GPU_DrawTris(&d, b->v, b->n);
    GPU_SetMatrix(mvp);
}

static void DrawSpriteList(sprite_t *list, int n, boolean screen)
{
    gpu_draw_t d;
    int pass, i;

    // opaque ones first (any order: the depth buffer sorts them), then the
    // see-through ones far to near without writing depth
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < n; i++)
        {
            sprite_t *s = &list[i];

            if ((s->blend != GPU_BLEND_NONE) != (pass == 1))
                continue;

            memset(&d, 0, sizeof(d));
            d.tex = s->tex;
            d.prog = GPU_PROG_WORLD;
            d.blend = s->blend;
            d.depth_test = !screen;
            d.depth_write = !screen && s->blend == GPU_BLEND_NONE;
            d.color_write = 1;
            d.fixed_row = (float)s->fixed;
            GPU_DrawTris(&d, s->v, 6);
        }
    }
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

    G_MirrorProjection(p);

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

static const lighttable_t *colormaps_uploaded;

boolean R_GPU_Active(void)
{
    return crispy_gpu_renderer && !r_gpu_softframe && I_GPU_Ready();
}

void R_GPU_NewLevel(void)
{
    // made on the first frame drawn by the GPU renderer; the textures
    // stay, most are shared between levels
    sspoly_level = NULL;

    // Hexen reads each level's fade table into the same colormaps
    colormaps_uploaded = NULL;
}

void R_GPU_FlushTextures(void)
{
    textures_stale = true;
}

void R_GPU_RenderPlayerView(player_t *player)
{
    double halffov;
    int i;

    R_SetupFrame(player);

    // [crispy] smooth texture scrolling
    R_InterpolateTextureOffsets();

    if (sspoly_level != subsectors)
    {
        BuildSubsectorPolys();
    }

    if (sector_frame_count != numsectors)
    {
        free(sector_frame);
        sector_frame = calloc(numsectors, sizeof(int));
        sector_frame_count = numsectors;
    }

    gpu_frame++;
    CheckTextureCache();

    // [AM] the sectors' interpolated heights (the software renderer does it
    // sector by sector as its BSP walk reaches them)
    for (i = 0; i < numsectors; i++)
    {
        G_InterpolateSector(&sectors[i]);
    }

    // the colormaps through the palette, for the fragment programs
    if (colormaps != colormaps_uploaded)
    {
        I_GPU_SetColormaps(colormaps, G_ColormapRows());
        colormaps_uploaded = colormaps;
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
    // The software renderer fills a pixel when its edge is reached (the
    // column or row starts at the ceiling of a position); the RSX tests
    // pixel centers. Half a pixel further, the pictures line up.
    cx = vw / 2.0f + 0.5f;
    cy = (float)centery + 0.5f;

    halffov = atan2(vw / 2.0, proj) + 3.0 * 3.14159265358979323846 / 180.0;
    gclipangle = (angle_t)(halffov / BAM2RAD);

    fixedrow = fixedcolormap ? (int)((fixedcolormap - colormaps) / 256) : -1;
    maxrow = (float)(NUMCOLORMAPS - 1);
    extralight_gpu = extralight;
    wallcap = (float)((47 * NONWIDEWIDTH / (int)(2.0f * proj)) / DISTMAP_GPU);
    sky_seen = false;

    memset(clipsolid, 0, sizeof(clipsolid));
    numsurfs = 0;
    surfverts.n = 0;
    numsprites_gpu = 0;
    numpspr = 0;
    numvisible_sectors = 0;

    if (numnodes)
        RenderBSPNode(numnodes - 1);
    else
        AddSubsector(0);

    CollectSprites();

    // the weapon: the engine's own code, its vissprites come back to
    // R_GPU_CaptureSprite
    r_gpu_capture = true;
    R_DrawPlayerSprites();
    r_gpu_capture = false;

    // to the RSX
    I_GPU_BeginView(viewwindowx, viewwindowy, scaledviewwidth, viewheight);
    SetupMatrix();
    GPU_SetMatrix(mvp);

    DrawSurfs();

    if (sky_seen)
    {
        G_DrawSky();
    }

    qsort(sprlist, numsprites_gpu, sizeof(*sprlist), CompareSprites);
    DrawSpriteList(sprlist, numsprites_gpu, false);

    SetOrtho(vw, vh);
    DrawSpriteList(pspr, numpspr, true);
    GPU_SetMatrix(mvp);

    I_GPU_EndView();
}

#endif // CRISPY_GPU
