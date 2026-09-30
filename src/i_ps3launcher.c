//
// i_ps3launcher.c -- game picker shown before the engine starts.
//
// The engine takes its IWAD from -iwad, which D_DoomMain consumes, so
// the choice has to be made before that call. That means nothing from
// the engine is available yet: no video (I_InitGraphics has not run),
// and in particular no font, since Doom's font lives inside the very
// WAD we are trying to pick. Hence the embedded 8x8 table below.
//
// The list is flat, the way the 2024 re-release presents things: every
// playable thing is one entry, whether it is a standalone IWAD or an
// add-on that needs a base game underneath. Picking "SIGIL" is really
// picking Ultimate Doom with SIGIL layered on -- SIGIL is nine maps,
// not a game, and has no sprites, textures or sounds of its own.
//
// Every entry launches with -noautoload, which i_main.c always passes.
// Crispy otherwise auto-loads SIGIL, SIGIL II, NERVE and MASTERLEVELS
// by looking for them next to the IWAD (D_LoadSigilWads, D_LoadNerveWad
// and D_LoadMasterlevelsWad in doom/d_pwad.c). On a console where every
// WAD sits in one directory that means choosing "The Ultimate DOOM"
// silently drags SIGIL in with it, title graphic and all. With the
// auto-load off, each entry loads exactly what it names and nothing
// else. The engine still recognises what arrives on -file:
// CheckNerveLoaded and CheckMasterlevelsLoaded run ahead of the
// auto-load path and set gamemission from a PWAD loaded that way.
//
// Which base game an add-on needs is read from its lump directory, not
// its filename: a WAD holding ExMy maps is episodic (Doom, Freedoom
// Phase 1, Chex), one holding MAPxx is Doom II family. SIGIL replaces
// E5Mx, so any ExMy match counts, not just E1M1.
//
// Mods (1.1): every subfolder of USRDIR/mods/ is one entry, listed on
// a separate MODS tab (L1/R1 or d-pad left/right switch tabs). A mod
// is whatever WADs and dehacked patches sit in its folder -- total
// conversions like Aliens TC come as several of each -- loaded as
// -file ... -deh ... on top of a base game picked the same way as for
// add-ons: by the maps inside, or by an optional mod.txt:
//
//     name = Aliens TC        label shown in the list
//     iwad = DOOM.WAD         base game, when the maps do not say
//     file = ALIENS.WAD       load order; with any file lines, only
//     file = ALIENS.DEH       the files listed are loaded
//
// Without mod.txt the WADs load in alphabetical order, then the
// patches. An IWAD inside the folder is taken as the base game itself
// (standalone games). Each mod saves to its own savegames/mod-<folder>
// via -savedir, so its saves never show up in the base game's menu.
//
// Video: the launcher brings up RSX itself and leaves it up.
// PS3_RSX_Init is not idempotent -- it memaligns a new IO buffer and
// calls rsxInit unconditionally -- so I_InitGraphics' later call is
// turned into a no-op by a guard there instead. rsx_src_mem is sized
// from SCREENWIDTH/SCREENHEIGHT at init time, so we pin those to the
// largest the engine can ask for (ORIGWIDTH/HEIGHT << 1, i.e. 640x400)
// before initialising. If the user runs with hires=0 the engine renders
// 320x200 into a buffer with room to spare, which PS3_RSX_Present
// handles because every dimension it uses comes from its parameters.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include <io/pad.h>

#include "doomtype.h"
#include "m_misc.h"
#include "i_video.h"

extern void PS3_Log(const char *fmt, ...);
extern void PS3_LogV(const char *fmt, ...);
extern void PS3_RSX_Init(void);
extern void PS3_RSX_Present(const u32 *src, int src_w, int src_h);

#define LAUNCHER_W   640
#define LAUNCHER_H   400
#define MAX_WADS     24
#define MAX_ENTRIES  48
#define MAX_FILES    16   // i_main.c's PS3_MAX_ADDONS must be as big
#define MAX_DEHS     8
#define MODS_DIR     PS3_USRDIR "/mods"
#define MAX_NAME     48
#define MAX_PATH_LEN 320
#define VISIBLE_ROWS 9
#define GLYPH_W      8
#define GLYPH_H      8
#define FONT_FIRST   32
#define FONT_LAST    95

#define COL_BG      0xff101018
#define COL_TEXT    0xffc8c8c0
#define COL_DIM     0xff707068
#define COL_SEL_BG  0xff803018
#define COL_SEL_TX  0xffffffff
#define COL_TITLE   0xffd08040

// 8x8 glyphs for ASCII 32..95 (printable, uppercase only -- lowercase
// is folded to uppercase at draw time).
static const unsigned char launcher_font[FONT_LAST - FONT_FIRST + 1][8] =
{
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // 'space'
    { 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00 },  // '!'
    { 0x36, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '"'
    { 0x36, 0x36, 0xfe, 0x36, 0xfe, 0x36, 0x36, 0x00 },  // '#'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '$'
    { 0xe2, 0xd2, 0xe4, 0x08, 0x3b, 0x2b, 0x3b, 0x00 },  // '%'
    { 0x38, 0x6c, 0x38, 0x76, 0xdc, 0xcc, 0x3d, 0x00 },  // '&'
    { 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '''
    { 0x0c, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0c, 0x00 },  // '('
    { 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x18, 0x30, 0x00 },  // ')'
    { 0x00, 0x2a, 0x1c, 0x3e, 0x1c, 0x2a, 0x00, 0x00 },  // '*'
    { 0x00, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00 },  // '+'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 },  // ','
    { 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00 },  // '-'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 },  // '.'
    { 0x02, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x40, 0x00 },  // '/'
    { 0x3c, 0x66, 0x6e, 0x7e, 0x76, 0x66, 0x3c, 0x00 },  // '0'
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00 },  // '1'
    { 0x3c, 0x66, 0x06, 0x0c, 0x18, 0x30, 0x7e, 0x00 },  // '2'
    { 0x7e, 0x0c, 0x18, 0x0c, 0x06, 0x66, 0x3c, 0x00 },  // '3'
    { 0x0e, 0x1e, 0x36, 0x66, 0x7f, 0x06, 0x0f, 0x00 },  // '4'
    { 0x7e, 0x60, 0x7c, 0x06, 0x06, 0x66, 0x3c, 0x00 },  // '5'
    { 0x1c, 0x30, 0x60, 0x7c, 0x66, 0x66, 0x3c, 0x00 },  // '6'
    { 0x7e, 0x66, 0x0c, 0x18, 0x18, 0x18, 0x18, 0x00 },  // '7'
    { 0x3c, 0x66, 0x66, 0x3c, 0x66, 0x66, 0x3c, 0x00 },  // '8'
    { 0x3c, 0x66, 0x66, 0x3e, 0x06, 0x0c, 0x38, 0x00 },  // '9'
    { 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00 },  // ':'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // ';'
    { 0x0e, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0e, 0x00 },  // '<'
    { 0x00, 0x00, 0x7e, 0x00, 0x7e, 0x00, 0x00, 0x00 },  // '='
    { 0x70, 0x18, 0x0c, 0x06, 0x0c, 0x18, 0x70, 0x00 },  // '>'
    { 0x3c, 0x66, 0x0c, 0x18, 0x18, 0x00, 0x18, 0x00 },  // '?'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '@'
    { 0x18, 0x3c, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x00 },  // 'A'
    { 0x7c, 0x66, 0x66, 0x7c, 0x66, 0x66, 0x7c, 0x00 },  // 'B'
    { 0x3c, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3c, 0x00 },  // 'C'
    { 0x78, 0x6c, 0x66, 0x66, 0x66, 0x6c, 0x78, 0x00 },  // 'D'
    { 0x7f, 0x63, 0x68, 0x78, 0x68, 0x63, 0x7f, 0x00 },  // 'E'
    { 0x7f, 0x63, 0x68, 0x78, 0x68, 0x60, 0x78, 0x00 },  // 'F'
    { 0x3c, 0x66, 0x60, 0x60, 0x67, 0x66, 0x3c, 0x00 },  // 'G'
    { 0x66, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x66, 0x00 },  // 'H'
    { 0x3c, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00 },  // 'I'
    { 0x07, 0x03, 0x03, 0x03, 0x63, 0x63, 0x3e, 0x00 },  // 'J'
    { 0x63, 0x66, 0x6c, 0x78, 0x6c, 0x66, 0x63, 0x00 },  // 'K'
    { 0x78, 0x60, 0x60, 0x60, 0x63, 0x63, 0x7f, 0x00 },  // 'L'
    { 0x63, 0x77, 0x7f, 0x6b, 0x63, 0x63, 0x63, 0x00 },  // 'M'
    { 0x63, 0x73, 0x7b, 0x6f, 0x67, 0x63, 0x63, 0x00 },  // 'N'
    { 0x3e, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00 },  // 'O'
    { 0x7e, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x78, 0x00 },  // 'P'
    { 0x3e, 0x63, 0x63, 0x63, 0x6b, 0x66, 0x3b, 0x00 },  // 'Q'
    { 0x7e, 0x63, 0x63, 0x7e, 0x6c, 0x66, 0x63, 0x00 },  // 'R'
    { 0x3e, 0x63, 0x70, 0x3e, 0x0e, 0x66, 0x3c, 0x00 },  // 'S'
    { 0x7f, 0x5a, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00 },  // 'T'
    { 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00 },  // 'U'
    { 0x63, 0x63, 0x63, 0x63, 0x63, 0x2c, 0x18, 0x00 },  // 'V'
    { 0x63, 0x63, 0x63, 0x6b, 0x7f, 0x77, 0x63, 0x00 },  // 'W'
    { 0x63, 0x63, 0x2c, 0x18, 0x2c, 0x63, 0x63, 0x00 },  // 'X'
    { 0x63, 0x63, 0x63, 0x3e, 0x0c, 0x18, 0x3c, 0x00 },  // 'Y'
    { 0x7f, 0x63, 0x0c, 0x18, 0x30, 0x63, 0x7f, 0x00 },  // 'Z'
    { 0x3c, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3c, 0x00 },  // '['
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '\'
    { 0x3c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x3c, 0x00 },  // ']'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '^'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff },  // '_'
};

typedef enum
{
    FAMILY_UNKNOWN = 0,
    FAMILY_EPISODIC,   // ExMy maps: Doom, Freedoom Phase 1, Chex, SIGIL
    FAMILY_MAPXX       // MAPxx maps: Doom II, TNT, Plutonia, NERVE
} wad_family_t;

typedef struct
{
    char         path[MAX_PATH_LEN];
    char         file[MAX_NAME];
    char         label[MAX_NAME];
    wad_family_t family;
    boolean      is_iwad;
} wad_info_t;

// One selectable thing. A base game has num_files == 0; an add-on
// carries the PWADs that go on -file, in load order.
typedef struct
{
    char label[MAX_NAME];
    char iwad[MAX_PATH_LEN];
    char files[MAX_FILES][MAX_PATH_LEN];
    int  num_files;
    // Mods only (USRDIR/mods/<folder>/):
    boolean is_mod;
    char base_label[MAX_NAME];               // shown beside the name
    char dehs[MAX_DEHS][MAX_PATH_LEN];       // -deh, in load order
    int  num_dehs;
    char savedir[MAX_PATH_LEN];              // -savedir
} game_entry_t;

static wad_info_t   found[MAX_WADS];
static int          num_found = 0;
static game_entry_t entries[MAX_ENTRIES];
static int          num_entries = 0;
static int          chosen_entry = 0;
static u32         *fb = NULL;

// ---------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------

static void FillRect(int x, int y, int w, int h, u32 colour)
{
    int px, py;

    for (py = y; py < y + h; py++)
    {
        if (py < 0 || py >= LAUNCHER_H)
            continue;
        for (px = x; px < x + w; px++)
        {
            if (px < 0 || px >= LAUNCHER_W)
                continue;
            fb[py * LAUNCHER_W + px] = colour;
        }
    }
}

// scale is a whole-pixel multiplier: 1 gives 8x8, 2 gives 16x16. At
// 640x400 upscaled to 720p, scale 2 is about the smallest that stays
// legible on a CRT.
static void DrawChar(int x, int y, char c, u32 colour, int scale)
{
    const unsigned char *glyph;
    int row, col, sx, sy;

    if (c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';

    if (c < FONT_FIRST || c > FONT_LAST)
        c = '?';

    glyph = launcher_font[c - FONT_FIRST];

    for (row = 0; row < GLYPH_H; row++)
    {
        for (col = 0; col < GLYPH_W; col++)
        {
            if ((glyph[row] & (0x80 >> col)) == 0)
                continue;

            for (sy = 0; sy < scale; sy++)
            {
                for (sx = 0; sx < scale; sx++)
                {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;

                    if (px >= 0 && px < LAUNCHER_W
                     && py >= 0 && py < LAUNCHER_H)
                    {
                        fb[py * LAUNCHER_W + px] = colour;
                    }
                }
            }
        }
    }
}

static void DrawText(int x, int y, const char *s, u32 colour, int scale)
{
    while (*s != '\0')
    {
        DrawChar(x, y, *s, colour, scale);
        x += GLYPH_W * scale;
        s++;
    }
}

static int TextWidth(const char *s, int scale)
{
    return (int) strlen(s) * GLYPH_W * scale;
}

// ---------------------------------------------------------------------
// WAD inspection
// ---------------------------------------------------------------------

// WAD header: 4-byte magic, then numlumps and infotableofs as 32-bit
// little-endian. The PS3 is big-endian, so those two need assembling
// byte by byte -- the same reason i_swap.h exists for the engine.
static uint32_t ReadLE32(const unsigned char *p)
{
    return (uint32_t) p[0]
         | ((uint32_t) p[1] << 8)
         | ((uint32_t) p[2] << 16)
         | ((uint32_t) p[3] << 24);
}

static boolean IsEpisodicName(const char *n)
{
    return n[0] == 'E' && n[1] >= '1' && n[1] <= '9'
        && n[2] == 'M' && n[3] >= '1' && n[3] <= '9';
}

static boolean IsMapName(const char *n)
{
    return n[0] == 'M' && n[1] == 'A' && n[2] == 'P'
        && n[3] >= '0' && n[3] <= '9'
        && n[4] >= '0' && n[4] <= '9';
}

// Fills in magic and family. Returns false for anything that is not a
// WAD at all, so junk in the directory is skipped silently.
static boolean InspectWad(const char *path, boolean *is_iwad,
                          wad_family_t *family)
{
    unsigned char header[12];
    unsigned char entry[16];
    uint32_t numlumps, infotableofs, i;
    FILE *fh;

    *family = FAMILY_UNKNOWN;

    fh = fopen(path, "rb");
    if (fh == NULL)
    {
        return false;
    }

    if (fread(header, 1, 12, fh) != 12)
    {
        fclose(fh);
        return false;
    }

    if (memcmp(header, "IWAD", 4) == 0)
    {
        *is_iwad = true;
    }
    else if (memcmp(header, "PWAD", 4) == 0)
    {
        *is_iwad = false;
    }
    else
    {
        fclose(fh);
        return false;
    }

    numlumps     = ReadLE32(header + 4);
    infotableofs = ReadLE32(header + 8);

    // A malformed header could claim anything; cap the walk rather than
    // trusting the count.
    if (numlumps > 65536)
    {
        numlumps = 65536;
    }

    if (fseek(fh, (long) infotableofs, SEEK_SET) != 0)
    {
        fclose(fh);
        return true;   // valid magic, family stays unknown
    }

    for (i = 0; i < numlumps; i++)
    {
        char name[9];

        if (fread(entry, 1, 16, fh) != 16)
        {
            break;
        }

        memcpy(name, entry + 8, 8);
        name[8] = '\0';

        if (IsEpisodicName(name))
        {
            *family = FAMILY_EPISODIC;
            break;
        }

        if (IsMapName(name))
        {
            *family = FAMILY_MAPXX;
            break;
        }
    }

    fclose(fh);
    return true;
}

// ---------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------

// Per-engine tables. Each PKG builds one engine (PS3_GAME in
// ps3/appid.mk), and an engine can only run its own IWADs -- Heretic
// calls D_FindIWAD(IWAD_MASK_HERETIC), so handing it doom2.wad would
// just produce an error. Listing only what this build can run keeps the
// menu honest.
#if defined(PS3_GAME_HERETIC)

static const struct { const char *file; const char *label; } known_names[] =
{
    { "HERETIC.WAD",       "Heretic: Shadow of the Serpent Riders" },
    { "HERETIC1.WAD",      "Heretic (Shareware)" },
    { NULL, NULL }
};

static const struct { const char *with; const char *music; } companions[] =
{
    { NULL, NULL }
};

static const struct { const char *pwad; const char *iwad; } required_base[] =
{
    { NULL, NULL }
};

static const char *const episodic_bases[] = { "HERETIC.WAD", "HERETIC1.WAD", NULL };
static const char *const mapxx_bases[]    = { NULL };

#elif defined(PS3_GAME_STRIFE)

static const struct { const char *file; const char *label; } known_names[] =
{
    { "STRIFE1.WAD",       "Strife: Quest for the Sigil" },
    { NULL, NULL }
};

// VOICES.WAD holds the spoken dialogue. It is a PWAD and not playable
// on its own, so it rides along with the IWAD instead of showing up as
// its own entry -- same treatment SIGIL_SHREDS.WAD gets.
static const struct { const char *with; const char *music; } companions[] =
{
    { "STRIFE1.WAD",       "VOICES.WAD" },
    { NULL, NULL }
};

static const struct { const char *pwad; const char *iwad; } required_base[] =
{
    { NULL, NULL }
};

static const char *const episodic_bases[] = { NULL };
static const char *const mapxx_bases[]    = { "STRIFE1.WAD", NULL };

#elif defined(PS3_GAME_HEXEN)

static const struct { const char *file; const char *label; } known_names[] =
{
    { "HEXEN.WAD",         "Hexen: Beyond Heretic" },
    { "HEXDD.WAD",         "Hexen: Deathkings of the Dark Citadel" },
    { NULL, NULL }
};

static const struct { const char *with; const char *music; } companions[] =
{
    { NULL, NULL }
};

static const struct { const char *pwad; const char *iwad; } required_base[] =
{
    { "HEXDD.WAD",         "HEXEN.WAD" },
    { NULL, NULL }
};

static const char *const episodic_bases[] = { NULL };
static const char *const mapxx_bases[]    = { "HEXEN.WAD", NULL };

#else  // PS3_GAME_DOOM

static const struct { const char *file; const char *label; } known_names[] =
{
    { "DOOM2.WAD",         "DOOM II: Hell on Earth" },
    { "PLUTONIA.WAD",      "Final Doom: Plutonia" },
    { "TNT.WAD",           "Final Doom: TNT Evilution" },
    { "DOOM.WAD",          "The Ultimate DOOM" },
    { "DOOM1.WAD",         "DOOM (Shareware)" },
    { "DOOM2F.WAD",        "DOOM II (French)" },
    { "FREEDOOM1.WAD",     "Freedoom: Phase 1" },
    { "FREEDOOM2.WAD",     "Freedoom: Phase 2" },
    { "FREEDM.WAD",        "FreeDM" },
    { "CHEX.WAD",          "Chex Quest" },
    { "CHEX2.WAD",         "Chex Quest 2" },
    { "CHEX3.WAD",         "Chex Quest 3" },
    { "HACX.WAD",          "HacX" },
    { "SIGIL.WAD",         "SIGIL" },
    { "SIGIL_V1_21.WAD",   "SIGIL" },
    { "SIGIL2.WAD",        "SIGIL II" },
    { "SIGIL_II_V1_0.WAD", "SIGIL II" },
    { "NERVE.WAD",         "No Rest for the Living" },
    { "MASTERLEVELS.WAD",  "Master Levels" },
    { NULL, NULL }
};

// Soundtrack WADs that are not playable on their own -- they replace
// the music of another add-on. Loaded alongside it automatically
// instead of cluttering the list with an entry that does nothing. With
// -noautoload the engine will not pull these in by itself any more, so
// pairing them here is what keeps that music working.
static const struct { const char *with; const char *music; } companions[] =
{
    { "SIGIL.WAD",         "SIGIL_SHREDS.WAD" },
    { "SIGIL_V1_21.WAD",   "SIGIL_SHREDS.WAD" },
    { "SIGIL2.WAD",        "SIGIL_II_MP3.WAD" },
    { "SIGIL_II_V1_0.WAD", "SIGIL_II_MP3.WAD" },
    { NULL, NULL }
};

#endif  // per-engine tables

static const char *LabelFor(const char *filename)
{
    int i;

    for (i = 0; known_names[i].file != NULL; i++)
    {
        if (strcasecmp(filename, known_names[i].file) == 0)
        {
            return known_names[i].label;
        }
    }

    return filename;
}

static boolean IsCompanion(const char *filename)
{
    int i;

    for (i = 0; companions[i].with != NULL; i++)
    {
        // Guard the second field too: on engines whose table is just
        // the NULL terminator, the compiler cannot see that the loop
        // never runs, and a half-filled row would be a silent crash.
        if (companions[i].music != NULL
         && strcasecmp(filename, companions[i].music) == 0)
        {
            return true;
        }
    }

    return false;
}

static const char *CompanionOf(const char *filename)
{
    int i;

    for (i = 0; companions[i].with != NULL; i++)
    {
        if (strcasecmp(filename, companions[i].with) == 0)
        {
            return companions[i].music;
        }
    }

    return NULL;
}

// ---------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------

static boolean HasWadExtension(const char *name)
{
    size_t len = strlen(name);

    return len > 4 && strcasecmp(name + len - 4, ".WAD") == 0;
}

static wad_info_t *FindByName(const char *filename)
{
    int i;

    for (i = 0; i < num_found; i++)
    {
        if (strcasecmp(found[i].file, filename) == 0)
        {
            return &found[i];
        }
    }

    return NULL;
}

static void ScanDir(const char *dir)
{
    DIR *d;
    struct dirent *e;

    d = opendir(dir);
    if (d == NULL)
    {
        return;
    }

    while ((e = readdir(d)) != NULL && num_found < MAX_WADS)
    {
        char full[MAX_PATH_LEN];
        wad_info_t *w;

        if (!HasWadExtension(e->d_name) || FindByName(e->d_name) != NULL)
        {
            continue;
        }

        M_snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);

        w = &found[num_found];
        memset(w, 0, sizeof(*w));

        if (!InspectWad(full, &w->is_iwad, &w->family))
        {
            continue;
        }

        M_StringCopy(w->path, full, sizeof(w->path));
        M_StringCopy(w->file, e->d_name, sizeof(w->file));
        M_StringCopy(w->label, LabelFor(e->d_name), sizeof(w->label));

        PS3_LogV("Launcher: %s '%s' family=%d (%s)",
                w->is_iwad ? "IWAD" : "PWAD", e->d_name,
                (int) w->family, w->label);

        num_found++;
    }

    closedir(d);
}

// Add-ons that need one specific IWAD, not just any of their family.
// CHEX2.WAD is the case that forced this: it is episodic, so the family
// tables below would hand it DOOM.WAD, and because it replaces maps and
// graphics but no sounds you get Chex visuals over Doom audio. Family
// is the fallback, this is the override.
#if !defined(PS3_GAME_HERETIC) && !defined(PS3_GAME_HEXEN) \
 && !defined(PS3_GAME_STRIFE)
static const struct { const char *pwad; const char *iwad; } required_base[] =
{
    { "CHEX2.WAD",         "CHEX.WAD" },
    { "CHEX3.WAD",         "CHEX.WAD" },
    { "SIGIL.WAD",         "DOOM.WAD" },
    { "SIGIL_V1_21.WAD",   "DOOM.WAD" },
    { "SIGIL2.WAD",        "DOOM.WAD" },
    { "SIGIL_II_V1_0.WAD", "DOOM.WAD" },
    { "NERVE.WAD",         "DOOM2.WAD" },
    { "MASTERLEVELS.WAD",  "DOOM2.WAD" },
    { NULL, NULL }
};

// Preferred base game per family, best first. Order matters: with both
// DOOM.WAD and DOOM1.WAD installed, an episodic add-on has to land on
// the retail IWAD -- the shareware one only ships episode 1, so SIGIL's
// E5 maps would load against missing assets.
static const char *const episodic_bases[] = {
    "DOOM.WAD", "FREEDOOM1.WAD", "CHEX.WAD", "DOOM1.WAD", NULL
};

static const char *const mapxx_bases[] = {
    "DOOM2.WAD", "DOOM2F.WAD", "FREEDOOM2.WAD", "TNT.WAD",
    "PLUTONIA.WAD", "HACX.WAD", "FREEDM.WAD", NULL
};

#endif  // Doom-only tables

// Pick the base game for an add-on: the best installed IWAD of the same
// family by the tables above, falling back to any IWAD of that family
// for names we do not know. Returns NULL when the user has the add-on
// but no game it can run on, in which case the entry is dropped rather
// than offered as something that would fail on launch.
// Returns the IWAD an add-on insists on, or NULL when it has no such
// requirement and family matching is good enough.
static const char *RequiredBaseFor(const char *pwad)
{
    int i;

    for (i = 0; required_base[i].pwad != NULL; i++)
    {
        if (strcasecmp(pwad, required_base[i].pwad) == 0)
        {
            return required_base[i].iwad;
        }
    }

    return NULL;
}

static wad_info_t *BaseGameFor(wad_family_t family)
{
    const char *const *prefs;
    int i, p;

    prefs = (family == FAMILY_EPISODIC) ? episodic_bases : mapxx_bases;

    for (p = 0; prefs[p] != NULL; p++)
    {
        for (i = 0; i < num_found; i++)
        {
            if (found[i].is_iwad && found[i].family == family
             && strcasecmp(found[i].file, prefs[p]) == 0)
            {
                return &found[i];
            }
        }
    }

    for (i = 0; i < num_found; i++)
    {
        if (found[i].is_iwad && found[i].family == family)
        {
            return &found[i];
        }
    }

    return NULL;
}

static void BuildEntries(void)
{
    int i;

    num_entries = 0;

    // Base games first, in the order they were found.
    for (i = 0; i < num_found && num_entries < MAX_ENTRIES; i++)
    {
        game_entry_t *g;

        // Companions are never games of their own, whatever their header
        // says: Strife's VOICES.WAD is marked IWAD, and listing it made
        // an entry that cannot start (the engine loads it by itself,
        // from next to STRIFE1.WAD).
        if (!found[i].is_iwad || IsCompanion(found[i].file))
        {
            continue;
        }

        g = &entries[num_entries++];
        memset(g, 0, sizeof(*g));
        M_StringCopy(g->label, found[i].label, sizeof(g->label));
        M_StringCopy(g->iwad, found[i].path, sizeof(g->iwad));
    }

    // Then add-ons, each already paired with a base game.
    for (i = 0; i < num_found && num_entries < MAX_ENTRIES; i++)
    {
        wad_info_t *base;
        wad_info_t *music;
        wad_info_t *prereq;
        const char *companion;
        const char *required;
        game_entry_t *g;

        if (found[i].is_iwad || IsCompanion(found[i].file))
        {
            continue;
        }

        // A named requirement wins over family matching, since family
        // alone would hand CHEX2.WAD to DOOM.WAD and give you Chex
        // graphics over Doom audio.
        required = RequiredBaseFor(found[i].file);
        prereq = NULL;

        if (required != NULL)
        {
            wad_info_t *req = FindByName(required);

            if (req == NULL)
            {
                PS3_LogV("Launcher: skipping '%s', needs %s",
                        found[i].file, required);
                continue;
            }

            if (req->is_iwad)
            {
                base = req;
            }
            else
            {
                // The prerequisite is an add-on itself: some Chex Quest
                // releases ship CHEX.WAD as a PWAD over Doom rather
                // than as its own IWAD. Then it has to be loaded first,
                // on top of whatever base it needs, and the chain ends
                // up as -iwad DOOM.WAD -file CHEX.WAD CHEX2.WAD.
                prereq = req;
                base = BaseGameFor(req->family);

                if (base == NULL)
                {
                    PS3_LogV("Launcher: skipping '%s', no game to run %s on",
                            found[i].file, required);
                    continue;
                }
            }
        }
        else
        {
            base = BaseGameFor(found[i].family);

            if (base == NULL)
            {
                PS3_LogV("Launcher: skipping '%s', no compatible game installed",
                        found[i].file);
                continue;
            }
        }

        g = &entries[num_entries++];
        memset(g, 0, sizeof(*g));
        M_StringCopy(g->label, found[i].label, sizeof(g->label));
        M_StringCopy(g->iwad, base->path, sizeof(g->iwad));

        // Load order matters: the prerequisite goes on -file before the
        // add-on that needs it, so the later one wins on clashing lumps.
        if (prereq != NULL)
        {
            M_StringCopy(g->files[g->num_files++], prereq->path,
                         sizeof(g->files[0]));
        }

        M_StringCopy(g->files[g->num_files++], found[i].path,
                     sizeof(g->files[0]));

        companion = CompanionOf(found[i].file);
        if (companion != NULL)
        {
            music = FindByName(companion);
            if (music != NULL && g->num_files < MAX_FILES)
            {
                M_StringCopy(g->files[g->num_files++], music->path,
                             sizeof(g->files[0]));
                PS3_LogV("Launcher: '%s' also loads '%s'",
                        found[i].file, companion);
            }
        }

        PS3_LogV("Launcher: entry '%s' = %s + %s",
                g->label, base->file, found[i].file);
    }
}

// ---------------------------------------------------------------------
// Mods (USRDIR/mods/<folder>/)
// ---------------------------------------------------------------------

// Dehacked patch extensions this engine understands. Hexen has no
// dehacked support at all (h2_main.c never calls DEH_ParseCommandLine),
// so there patches are ignored rather than passed along.
static boolean IsDehFile(const char *name)
{
    size_t len = strlen(name);

    if (len <= 4)
    {
        return false;
    }

    name += len - 4;

#if defined(PS3_GAME_HEXEN)
    (void) name;
    return false;
#elif defined(PS3_GAME_HERETIC)
    return strcasecmp(name, ".HHE") == 0 || strcasecmp(name, ".DEH") == 0;
#else
    return strcasecmp(name, ".DEH") == 0 || strcasecmp(name, ".BEX") == 0;
#endif
}

static int CompareNames(const void *a, const void *b)
{
    return strcasecmp((const char *) a, (const char *) b);
}

// Trims leading/trailing blanks and the line ending, in place.
static char *Trim(char *str)
{
    char *end;

    while (*str == ' ' || *str == '\t')
    {
        str++;
    }

    end = str + strlen(str);
    while (end > str && (end[-1] == ' ' || end[-1] == '\t'
                      || end[-1] == '\r' || end[-1] == '\n'))
    {
        *--end = '\0';
    }

    return str;
}

// One mod folder while it is being put together. Static rather than on
// the stack: with its path arrays it is a few tens of KB, and the
// launcher runs on the game thread before anything else.
static struct
{
    char name[MAX_NAME];                     // mod.txt name =
    char iwad[MAX_NAME];                     // mod.txt iwad =
    char order[MAX_FILES + MAX_DEHS][MAX_NAME];  // mod.txt file =
    int  num_order;
    char wads[MAX_FILES][MAX_NAME];          // what the folder holds
    int  num_wads;
    char dehs[MAX_DEHS][MAX_NAME];
    int  num_dehs;
} mod;

static void ReadModTxt(const char *path)
{
    char line[256];
    FILE *fh;

    fh = fopen(path, "r");
    if (fh == NULL)
    {
        return;
    }

    while (fgets(line, sizeof(line), fh) != NULL)
    {
        char *key, *value, *eq;

        key = Trim(line);
        if (*key == '#' || *key == '\0')
        {
            continue;
        }

        eq = strchr(key, '=');
        if (eq == NULL)
        {
            continue;
        }

        *eq = '\0';
        key = Trim(key);
        value = Trim(eq + 1);

        if (*value == '\0')
        {
            continue;
        }

        if (strcasecmp(key, "name") == 0)
        {
            M_StringCopy(mod.name, value, sizeof(mod.name));
        }
        else if (strcasecmp(key, "iwad") == 0)
        {
            M_StringCopy(mod.iwad, value, sizeof(mod.iwad));
        }
        else if (strcasecmp(key, "file") == 0
              && mod.num_order < MAX_FILES + MAX_DEHS)
        {
            M_StringCopy(mod.order[mod.num_order++], value, sizeof(mod.order[0]));
        }
    }

    fclose(fh);
}

// Adds one file of the mod to the entry, as a WAD or as a patch by its
// extension. Returns false (and logs) when the entry is already full.
static boolean AddModFile(game_entry_t *g, const char *dir, const char *file)
{
    char full[MAX_PATH_LEN];

    M_snprintf(full, sizeof(full), "%s/%s", dir, file);

    if (IsDehFile(file))
    {
        if (g->num_dehs >= MAX_DEHS)
        {
            PS3_Log("Launcher: mod '%s': too many patches, dropping '%s'",
                    g->label, file);
            return false;
        }
        M_StringCopy(g->dehs[g->num_dehs++], full, sizeof(g->dehs[0]));
    }
    else
    {
        if (g->num_files >= MAX_FILES)
        {
            PS3_Log("Launcher: mod '%s': too many WADs, dropping '%s'",
                    g->label, file);
            return false;
        }
        M_StringCopy(g->files[g->num_files++], full, sizeof(g->files[0]));
    }

    return true;
}

// Finds a file of the folder listing by name, ignoring case. The PS3
// filesystem is case-sensitive, so mod.txt is matched against what is
// really there and the real spelling is what goes on the command line.
static const char *FindModFile(const char *name)
{
    int i;

    for (i = 0; i < mod.num_wads; i++)
    {
        if (strcasecmp(mod.wads[i], name) == 0)
            return mod.wads[i];
    }

    for (i = 0; i < mod.num_dehs; i++)
    {
        if (strcasecmp(mod.dehs[i], name) == 0)
            return mod.dehs[i];
    }

    return NULL;
}

static void BuildModEntry(const char *folder, const char *dir)
{
    wad_info_t   base_in_folder;
    wad_info_t  *base = NULL;
    wad_family_t family = FAMILY_UNKNOWN;
    const char  *base_file = NULL;    // the folder's own IWAD, if used
    game_entry_t *g;
    char full[MAX_PATH_LEN];
    int i;

    memset(&base_in_folder, 0, sizeof(base_in_folder));

    if (mod.num_wads == 0)
    {
        PS3_Log("Launcher: mod '%s' has no WADs, skipped", folder);
        return;
    }

    // Look inside the WADs: an IWAD makes this a standalone game, and
    // the first one with maps says which family of base game it needs.
    for (i = 0; i < mod.num_wads; i++)
    {
        boolean is_iwad = false;
        wad_family_t fam;

        M_snprintf(full, sizeof(full), "%s/%s", dir, mod.wads[i]);

        if (!InspectWad(full, &is_iwad, &fam))
        {
            continue;
        }

        if (is_iwad && base_file == NULL)
        {
            base_file = mod.wads[i];
            M_StringCopy(base_in_folder.path, full, sizeof(base_in_folder.path));
            M_StringCopy(base_in_folder.file, mod.wads[i],
                         sizeof(base_in_folder.file));
            M_StringCopy(base_in_folder.label, mod.wads[i],
                         sizeof(base_in_folder.label));
            base_in_folder.is_iwad = true;
            base_in_folder.family = fam;
        }

        if (family == FAMILY_UNKNOWN && fam != FAMILY_UNKNOWN)
        {
            family = fam;
        }
    }

    // 1. iwad = in mod.txt: the folder's own IWAD, or an installed one.
    if (mod.iwad[0] != '\0')
    {
        if (base_file != NULL && strcasecmp(mod.iwad, base_file) == 0)
        {
            base = &base_in_folder;
        }
        else
        {
            base = FindByName(mod.iwad);

            if (base == NULL || !base->is_iwad)
            {
                PS3_Log("Launcher: mod '%s' needs %s, not installed",
                        folder, mod.iwad);
                return;
            }

            base_file = NULL;   // the folder's IWAD, if any, loads as -file
        }
    }
    // 2. an IWAD inside the folder: a standalone game.
    else if (base_file != NULL)
    {
        base = &base_in_folder;
    }
    // 3. the maps say which family; 4. otherwise any base game at all,
    // so mods that only replace graphics or sounds still get listed.
    else
    {
        if (family != FAMILY_UNKNOWN)
        {
            base = BaseGameFor(family);
        }
        if (base == NULL)
        {
            base = BaseGameFor(FAMILY_MAPXX);
        }
        if (base == NULL)
        {
            base = BaseGameFor(FAMILY_EPISODIC);
        }
        if (base == NULL)
        {
            for (i = 0; i < num_found; i++)
            {
                if (found[i].is_iwad)
                {
                    base = &found[i];
                    break;
                }
            }
        }
    }

    if (base == NULL)
    {
        PS3_Log("Launcher: mod '%s' has no game to run on, skipped", folder);
        return;
    }

    if (num_entries >= MAX_ENTRIES)
    {
        PS3_Log("Launcher: too many entries, mod '%s' skipped", folder);
        return;
    }

    g = &entries[num_entries];
    memset(g, 0, sizeof(*g));
    g->is_mod = true;
    M_StringCopy(g->iwad, base->path, sizeof(g->iwad));
    M_StringCopy(g->base_label, base->label, sizeof(g->base_label));

    if (mod.name[0] != '\0')
    {
        M_StringCopy(g->label, mod.name, sizeof(g->label));
    }
    else
    {
        // Folder name, underscores read as spaces: ALIENS_TC -> ALIENS TC.
        char *c;

        M_StringCopy(g->label, folder, sizeof(g->label));
        for (c = g->label; *c != '\0'; c++)
        {
            if (*c == '_')
                *c = ' ';
        }
    }

    if (mod.num_order > 0)
    {
        // mod.txt decides: exactly these, in this order.
        for (i = 0; i < mod.num_order; i++)
        {
            const char *real = FindModFile(mod.order[i]);

            if (real == NULL)
            {
                PS3_Log("Launcher: mod '%s': '%s' from mod.txt not found",
                        folder, mod.order[i]);
                continue;
            }

            if (base_file != NULL && real == base_file)
            {
                continue;   // already the IWAD
            }

            AddModFile(g, dir, real);
        }
    }
    else
    {
        for (i = 0; i < mod.num_wads; i++)
        {
            if (base_file != NULL && mod.wads[i] == base_file)
            {
                continue;
            }
            AddModFile(g, dir, mod.wads[i]);
        }

        for (i = 0; i < mod.num_dehs; i++)
        {
            AddModFile(g, dir, mod.dehs[i]);
        }
    }

    M_snprintf(g->savedir, sizeof(g->savedir),
               "%s/savegames/mod-%s", PS3_USRDIR, folder);

    num_entries++;

    PS3_Log("Launcher: mod '%s' = %s + %d WAD(s), %d patch(es)",
            g->label, base->file, g->num_files, g->num_dehs);
}

static void ScanMods(void)
{
    DIR *top;
    struct dirent *e;
    // Folder names, collected first and sorted, so the tab has a stable
    // order instead of whatever order the filesystem hands out.
    static char folders[MAX_ENTRIES][MAX_NAME];
    int num_folders = 0;
    int f;

    top = opendir(MODS_DIR);
    if (top == NULL)
    {
        return;
    }

    while ((e = readdir(top)) != NULL && num_folders < MAX_ENTRIES)
    {
        if (e->d_name[0] == '.')
        {
            continue;
        }
        M_StringCopy(folders[num_folders++], e->d_name, sizeof(folders[0]));
    }

    closedir(top);

    qsort(folders, num_folders, sizeof(folders[0]), CompareNames);

    for (f = 0; f < num_folders; f++)
    {
        char dir[MAX_PATH_LEN];
        char txt[MAX_PATH_LEN];
        boolean have_txt = false;
        DIR *d;

        M_snprintf(dir, sizeof(dir), "%s/%s", MODS_DIR, folders[f]);

        // opendir doubles as the "is it a folder" test: plain files in
        // mods/ fail here and are ignored.
        d = opendir(dir);
        if (d == NULL)
        {
            continue;
        }

        memset(&mod, 0, sizeof(mod));

        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] == '.')
            {
                continue;
            }

            if (HasWadExtension(e->d_name))
            {
                if (mod.num_wads < MAX_FILES)
                    M_StringCopy(mod.wads[mod.num_wads++], e->d_name,
                                 sizeof(mod.wads[0]));
            }
            else if (IsDehFile(e->d_name))
            {
                if (mod.num_dehs < MAX_DEHS)
                    M_StringCopy(mod.dehs[mod.num_dehs++], e->d_name,
                                 sizeof(mod.dehs[0]));
            }
            else if (strcasecmp(e->d_name, "mod.txt") == 0)
            {
                M_snprintf(txt, sizeof(txt), "%s/%s", dir, e->d_name);
                have_txt = true;
            }
        }

        closedir(d);

        qsort(mod.wads, mod.num_wads, sizeof(mod.wads[0]), CompareNames);
        qsort(mod.dehs, mod.num_dehs, sizeof(mod.dehs[0]), CompareNames);

        if (have_txt)
        {
            ReadModTxt(txt);
        }

        BuildModEntry(folders[f], dir);
    }
}

// ---------------------------------------------------------------------
// TV Screen Fit
// ---------------------------------------------------------------------

// The launcher draws before the engine has read its config, so pick up
// ps3_screen_fit from the cfg here -- otherwise a TV that needed 80 %
// would still crop the launcher at the default. Same file M_LoadDefaults
// reads it from later (the "extra" crispy-<game>.cfg).
static void LoadScreenFit(void)
{
#if defined(PS3_GAME_HERETIC)
    const char *cfg = PS3_USRDIR "/crispy-heretic.cfg";
#elif defined(PS3_GAME_HEXEN)
    const char *cfg = PS3_USRDIR "/crispy-hexen.cfg";
#elif defined(PS3_GAME_STRIFE)
    const char *cfg = PS3_USRDIR "/crispy-strife.cfg";
#else
    const char *cfg = PS3_USRDIR "/crispy-doom.cfg";
#endif
    char line[128];
    FILE *fh;

    fh = fopen(cfg, "r");
    if (fh == NULL)
    {
        return;
    }

    while (fgets(line, sizeof(line), fh) != NULL)
    {
        int value;

        if (sscanf(line, "ps3_screen_fit %d", &value) == 1)
        {
            ps3_screen_fit = value < PS3_SCREEN_FIT_MIN ? PS3_SCREEN_FIT_MIN
                           : value > PS3_SCREEN_FIT_MAX ? PS3_SCREEN_FIT_MAX
                           : value;
            break;
        }
    }

    fclose(fh);
}

// ---------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------

#define PAD_LEFT   32768
#define PAD_UP     4096
#define PAD_RIGHT  8192
#define PAD_DOWN   16384
#define PAD_CROSS  64
#define PAD_START  2048
#define PAD_L1     4
#define PAD_R1     8

static padInfo pad_info;
static padData pad_data;

static unsigned ReadPad(void)
{
    int i;

    ioPadGetInfo(&pad_info);

    for (i = 0; i < MAX_PADS; i++)
    {
        if (pad_info.status[i])
        {
            ioPadGetData(i, &pad_data);
            return (pad_data.button[2] << 8) | (pad_data.button[3] & 0xff);
        }
    }

    return 0;
}

// ---------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------

// Two tabs: base games and add-ons (what 1.0 listed), and mods. The
// tab row only appears when there is at least one mod; with none the
// launcher looks exactly like 1.0.
#define TAB_GAMES 0
#define TAB_MODS  1

static int tab_items[2][MAX_ENTRIES];   // indices into entries[]
static int tab_count[2];

static void BuildTabs(void)
{
    int i;

    tab_count[TAB_GAMES] = tab_count[TAB_MODS] = 0;

    for (i = 0; i < num_entries; i++)
    {
        int t = entries[i].is_mod ? TAB_MODS : TAB_GAMES;
        tab_items[t][tab_count[t]++] = i;
    }
}

static void DrawMenu(int tab, int selected, int top)
{
#if defined(PS3_GAME_HERETIC)
    const char *title = "CRISPY HERETIC";
#elif defined(PS3_GAME_STRIFE)
    const char *title = "CRISPY STRIFE";
#elif defined(PS3_GAME_HEXEN)
    const char *title = "CRISPY HEXEN";
#else
    const char *title = "CRISPY DOOM";
#endif
    const boolean tabs = tab_count[TAB_MODS] > 0 && tab_count[TAB_GAMES] > 0;
    const char *hint = tabs ? "D-PAD CHOOSE   L1/R1 GAMES/MODS   CROSS PLAY"
                            : "D-PAD TO CHOOSE    CROSS TO PLAY";
    const int count = tab_count[tab];
    int row_h = 26;
    int i;

    FillRect(0, 0, LAUNCHER_W, LAUNCHER_H, COL_BG);
    DrawText((LAUNCHER_W - TextWidth(title, 3)) / 2, 34, title, COL_TITLE, 3);

    if (tabs)
    {
        static const char *const names[2] = { "GAMES", "MODS" };
        int x = LAUNCHER_W / 2 - TextWidth("GAMES", 2) - 24;
        int t;

        for (t = 0; t < 2; t++)
        {
            int w = TextWidth(names[t], 2);

            DrawText(x, 70, names[t], t == tab ? COL_TITLE : COL_DIM, 2);
            if (t == tab)
            {
                FillRect(x, 88, w, 2, COL_TITLE);
            }
            x += w + 48;
        }
    }

    for (i = 0; i < VISIBLE_ROWS && top + i < count; i++)
    {
        int idx = tab_items[tab][top + i];
        int y = 100 + i * row_h;
        u32 colour = COL_TEXT;

        if (top + i == selected)
        {
            FillRect(26, y - 4, LAUNCHER_W - 52, row_h - 4, COL_SEL_BG);
            colour = COL_SEL_TX;
        }

        DrawText(44, y, entries[idx].label, colour, 2);

        // A mod's base game, small and to the right, when it fits: it is
        // the one thing about a mod entry the launcher decided by itself.
        if (entries[idx].is_mod)
        {
            int lw = TextWidth(entries[idx].label, 2);
            int bw = TextWidth(entries[idx].base_label, 1);

            if (44 + lw + 16 + bw <= LAUNCHER_W - 40)
            {
                DrawText(LAUNCHER_W - 40 - bw, y + 4, entries[idx].base_label,
                         top + i == selected ? COL_SEL_TX : COL_DIM, 1);
            }
        }
    }

    // Only worth saying there is more when the list actually scrolls.
    if (count > VISIBLE_ROWS)
    {
        char counter[24];

        M_snprintf(counter, sizeof(counter), "%d / %d", selected + 1, count);
        DrawText(LAUNCHER_W - TextWidth(counter, 1) - 24,
                 LAUNCHER_H - 26, counter, COL_DIM, 1);
    }

    DrawText(24, LAUNCHER_H - 26, hint, COL_DIM, 1);
    PS3_RSX_Present(fb, LAUNCHER_W, LAUNCHER_H);
}

// Returns an index into entries[].
static int RunMenu(void)
{
    int tab = tab_count[TAB_GAMES] > 0 ? TAB_GAMES : TAB_MODS;
    int selected[2] = { 0, 0 };
    int top[2] = { 0, 0 };
    // Start as if everything were held: a button still down from the
    // XMB (cross launched the game) must not pick an entry by itself.
    unsigned last = ~0u;

    for (;;)
    {
        unsigned now = ReadPad();
        unsigned pressed = now & ~last;
        int count;

        last = now;

        if ((pressed & (PAD_L1 | PAD_R1 | PAD_LEFT | PAD_RIGHT))
         && tab_count[TAB_GAMES] > 0 && tab_count[TAB_MODS] > 0)
        {
            tab = (pressed & (PAD_L1 | PAD_LEFT)) ? TAB_GAMES : TAB_MODS;
        }

        count = tab_count[tab];

        if (pressed & PAD_UP)
        {
            selected[tab] = (selected[tab] + count - 1) % count;
        }

        if (pressed & PAD_DOWN)
        {
            selected[tab] = (selected[tab] + 1) % count;
        }

        if (pressed & (PAD_CROSS | PAD_START))
        {
            return tab_items[tab][selected[tab]];
        }

        if (selected[tab] < top[tab])
        {
            top[tab] = selected[tab];
        }
        else if (selected[tab] >= top[tab] + VISIBLE_ROWS)
        {
            top[tab] = selected[tab] - VISIBLE_ROWS + 1;
        }

        DrawMenu(tab, selected[tab], top[tab]);
    }
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

// A mod's -savedir. The engine creates the last level itself
// (M_GetSaveGameDir), but not the parent, so both are made here.
static void MakeSaveDir(const game_entry_t *g)
{
    if (g->savedir[0] == '\0')
    {
        return;
    }

    mkdir(PS3_USRDIR "/savegames", 0777);
    mkdir(g->savedir, 0777);
}

//
// Returns true and fills out_path with the IWAD to run. Returns false
// when nothing playable was found, leaving the caller to fall back to
// its own default so the engine's error path can report something.
//
// The menu is skipped when there is only one entry: making the user
// confirm a choice of one every launch would be pure friction.
//
boolean PS3_Launcher_SelectIWAD(char *out_path, size_t out_size)
{
    num_found = 0;
    chosen_entry = 0;

    ScanDir(PS3_USRDIR "/wads");
    ScanDir(PS3_USRDIR);

    BuildEntries();
    ScanMods();
    BuildTabs();

    if (num_entries == 0)
    {
        PS3_Log("Launcher: nothing playable found");
        return false;
    }

    if (num_entries == 1)
    {
        PS3_Log("Launcher: one entry, skipping menu");
        MakeSaveDir(&entries[0]);
        M_StringCopy(out_path, entries[0].iwad, out_size);
        return true;
    }

    // Pin the render size to the largest the engine can request before
    // RSX allocates its source buffer -- see the file header comment.
    SCREENWIDTH  = ORIGWIDTH << 1;
    SCREENHEIGHT = ORIGHEIGHT << 1;

    fb = malloc(LAUNCHER_W * LAUNCHER_H * sizeof(u32));
    if (fb == NULL)
    {
        PS3_Log("Launcher: framebuffer alloc failed, using first entry");
        MakeSaveDir(&entries[0]);
        M_StringCopy(out_path, entries[0].iwad, out_size);
        return true;
    }

    LoadScreenFit();
    ioPadInit(MAX_PADS);
    PS3_RSX_Init();

    chosen_entry = RunMenu();

    free(fb);
    fb = NULL;

    PS3_Log("Launcher: chose '%s' (iwad %s, %d add-on files, %d patches)",
            entries[chosen_entry].label, entries[chosen_entry].iwad,
            entries[chosen_entry].num_files, entries[chosen_entry].num_dehs);

    MakeSaveDir(&entries[chosen_entry]);
    M_StringCopy(out_path, entries[chosen_entry].iwad, out_size);
    return true;
}

//
// Fills paths with the PWADs the chosen entry needs on -file, in load
// order, and returns how many. Call only after
// PS3_Launcher_SelectIWAD -- the selection lives in this file's state.
//
int PS3_Launcher_GetAddons(const char **paths, int max)
{
    int i, n = 0;

    if (num_entries == 0)
    {
        return 0;
    }

    for (i = 0; i < entries[chosen_entry].num_files && n < max; i++)
    {
        paths[n++] = entries[chosen_entry].files[i];
    }

    return n;
}

//
// Same for the dehacked patches that go on -deh (mods only).
//
int PS3_Launcher_GetDehs(const char **paths, int max)
{
    int i, n = 0;

    if (num_entries == 0)
    {
        return 0;
    }

    for (i = 0; i < entries[chosen_entry].num_dehs && n < max; i++)
    {
        paths[n++] = entries[chosen_entry].dehs[i];
    }

    return n;
}

//
// The -savedir for the chosen entry, or NULL to keep the engine's own
// savegames/<iwad>/ (everything that is not a mod).
//
const char *PS3_Launcher_GetSaveDir(void)
{
    if (num_entries == 0 || entries[chosen_entry].savedir[0] == '\0')
    {
        return NULL;
    }

    return entries[chosen_entry].savedir;
}
