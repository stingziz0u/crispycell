//
// Copyright(C) 2026 CrispyCell
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//    [PS3] Music packs: recorded versions of the MIDI soundtrack (Ogg
//    Vorbis or MP3), dropped in a folder, played instead of the MIDI.
//
//    The same scheme as Chocolate/Crispy Doom's i_musicpack.c (which on
//    the PS3 has no SDL_mixer to play through): each song is found by
//    the SHA1 of the original MUS/MIDI lump, from
//      - the *.cfg files of the pack ("<sha1 or prefix> = <file>"), the
//        format of the chocolate-doom/music-packs configs, then
//      - the table of known file names (d_e1m1.ogg, mus_e1m1.ogg,
//        winnowr.ogg ...), so a folder of files needs no .cfg, and last
//      - the lump's own name (D_E1M1.ogg), for PWADs with new music.
//    Looked up in /dev_hdd0/data/crispycell/music/<game>/ first, then in
//    /dev_hdd0/data/crispycell/music/ (and a music/ folder inside each,
//    as the zipped packs come). Names are matched without case: the
//    PS3's hard disk is case-sensitive, the packs are not consistent.
//
//    .flac is not decoded here: a .cfg that names a .flac also tries the
//    same name in .ogg and .mp3. The loop points come from the file
//    (i_ps3music.c: LOOP_START/LOOP_END, LOOPSTART/LOOPLENGTH tags).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "doomtype.h"
#include "m_misc.h"
#include "sha1.h"
#include "w_wad.h"

extern void PS3_Log(const char *fmt, ...);

#define PACK_ROOT PS3_USRDIR "/music-packs"
#define PACK_SHARED "/dev_hdd0/data/crispycell/music"

#if defined(PS3_GAME_HERETIC)
#define PACK_GAME "heretic"
#elif defined(PS3_GAME_HEXEN)
#define PACK_GAME "hexen"
#elif defined(PS3_GAME_STRIFE)
#define PACK_GAME "strife"
#else
#define PACK_GAME "doom"
#endif

#define MAX_DIRS 32

typedef struct
{
    const char *hash_prefix;
    const char *filename;
} known_t;

// From i_musicpack.c (Chocolate Doom): the canonical file name of each
// original track, by the SHA1 of its lump.
static const known_t known_filenames[] = {
    // Doom 1 music files.
    {"b2e05b4e8dff8d76f8f4", "d_inter.{ext}"},
    {"0c0acce45130bab935d2", "d_intro.{ext}"},
    {"fca4086939a68ae4ed84", "d_victor.{ext}"},
    {"5971e5e20554f47ca065", "d_intro.{ext}"},
    {"99767e32769229897f77", "d_e1m1.{ext}"},
    {"b5e7dfb4efe9e688bf2a", "d_e1m2.{ext}"},
    {"fda8fa73e4d30a6b961c", "d_e1m3.{ext}"},
    {"3805f9bf3f1702f7e7f5", "d_e1m4.{ext}"},
    {"f546ed823b234fe39165", "d_e1m5.{ext}"},
    {"4450811b5a6748cfd83e", "d_e1m6.{ext}"},
    {"73edb50d96b0ac03be34", "d_e1m7.{ext}"},
    {"47d711a6fd32f5047879", "d_e1m8.{ext}"},
    {"62c631c2fdaa5ecd9a8d", "d_e1m9.{ext}"},
    {"7702a6449585428e7185", "d_e2m1.{ext}"},
    {"1cb1810989cbfae2b29b", "d_e2m2.{ext}"},
    {"7d740f3c881a22945e47", "d_e2m4.{ext}"},
    {"ae9c3dc2f9aeea002327", "d_e2m6.{ext}"},
    {"b26aad3caa420e9a2c76", "d_e2m7.{ext}"},
    {"90f06251a2a90bfaefd4", "d_e2m8.{ext}"},
    {"b2fb439f23c08c8e2577", "d_e3m1.{ext}"},
    {"b6c07bb249526b864208", "d_e3m2.{ext}"},
    {"ce3587ee503ffe707b2d", "d_e3m3.{ext}"},
    {"d746ea2aa16b3237422c", "d_e3m8.{ext}"},
    {"3da3b1335560a92912e6", "d_bunny.{ext}"},

    // Duplicates that don't have identical hashes:
    {"4a5badc4f10a7d4ed021", "d_inter.{ext}"},  // E2M3
    {"36b14bf165b3fdd3958e", "d_e1m7.{ext}"},   // E3M5
    {"e77c3d42f2ea87f04607", "d_e1m6.{ext}"},   // E3M6
    {"3d85ec9c10b5ea465568", "d_e2m7.{ext}"},   // E3M7
    {"4d42e2ce1c1ff192500e", "d_e1m9.{ext}"},   // E3M9

    // These tracks are reused in Alien Vendetta, but are MIDs:
    {"a05e45f67e1b64733fe3", "d_e2m1.{ext}"},   // MAP02
    {"8024ae1616ddd97ce330", "d_e1m4.{ext}"},   // MAP03
    {"3af8d79ddba49edaf9eb", "d_victor.{ext}"}, // MAP05
    {"a55352c96c025b6bd08a", "d_inter.{ext}"},  // MAP07
    {"76d1fc25ab7b1b4a58d6", "d_e1m8.{ext}"},   // MAP11
    {"497777f0863eca7cea87", "d_e1m2.{ext}"},   // MAP12
    {"0228fd87f8762f112fb6", "d_e2m2.{ext}"},   // MAP13
    {"db94e8e1d7c02092eab5", "d_e1m6.{ext}"},   // MAP14
    {"5a8d7a307eebc952795c", "d_e2m7.{ext}"},   // MAP16
    {"1a36b692bf26d94a72cc", "d_e1m7.{ext}"},   // MAP23
    {"37c6cefa351b06995152", "d_e1m5.{ext}"},   // MAP27
    {"36b97b87fe98348d44b6", "d_e2m6.{ext}"},   // MAP28

    // Doom II music files.
    {"79080e9681a2d7bec3fb", "d_runnin.{ext}"},  // MAP01,15
    {"868b3aae73c7b12e92c0", "d_stalks.{ext}"},  // MAP02,11,17
    {"19237754d2eb85f41d84", "d_countd.{ext}"},  // MAP03,21
    {"00abff3b61b25a6855d2", "d_betwee.{ext}"},  // MAP04
    {"954636c7ee09edf5d98f", "d_doom.{ext}"},    // MAP05,13
    {"8d32b2b7aa3b806474c1", "d_the_da.{ext}"},  // MAP06,12,24
    {"41efc3c84bb321af2b6b", "d_shawn.{ext}"},   // MAP07,19,29
    // Assuming single D_DDTBLU: http://doomwiki.org/wiki/Doom_II_music#Trivia
    {"51c0872fec9f43259318", "d_ddtblu.{ext}"},  // MAP08
    {"acb7ad85494d18235df8", "d_ddtblu.{ext}"},  // MAP14,22
    {"4b7ceccbf47e78e2fa0b", "d_in_cit.{ext}"},  // MAP09
    {"1d1f4a9edba174584e11", "d_dead.{ext}"},    // MAP10,16
    {"1736c81aac77f9bffd3d", "d_romero.{ext}"},  // MAP18,27
    {"a55d400570ad255a576b", "d_messag.{ext}"},  // MAP20,26
    {"29d30c3fbd712016f2e5", "d_ampie.{ext}"},   // MAP23
    {"bcfe9786afdcfb704afa", "d_adrian.{ext}"},  // MAP25
    {"e05c10389e71836834ae", "d_tense.{ext}"},   // MAP28
    {"b779022b1d0f0010b8f0", "d_openin.{ext}"},  // MAP30
    {"a9a5f7b0ab3be0f4fc24", "d_evil.{ext}"},    // MAP31
    {"4503d155aafec0296689", "d_ultima.{ext}"},  // MAP32
    {"56f2363f01df38908c77", "d_dm2ttl.{ext}"},
    {"71e58baf9e9dea4dd24a", "d_dm2int.{ext}"},
    {"e632318629869811f7dc", "d_read_m.{ext}"},

    // Duplicate filenames: the above filenames are the "canonical" files
    // for the given SHA1 hashes, but we can also look for these filenames
    // corresponding to the duplicated music tracks too.
    {"868b3aae73c7b12e92c0", "d_stlks2.{ext}"},
    {"868b3aae73c7b12e92c0", "d_stlks3.{ext}"},
    {"8d32b2b7aa3b806474c1", "d_theda2.{ext}"},
    {"8d32b2b7aa3b806474c1", "d_theda3.{ext}"},
    {"954636c7ee09edf5d98f", "d_doom2.{ext}"},
    {"acb7ad85494d18235df8", "d_ddtbl2.{ext}"},
    {"acb7ad85494d18235df8", "d_ddtbl3.{ext}"},
    {"79080e9681a2d7bec3fb", "d_runni2.{ext}"},
    {"1d1f4a9edba174584e11", "d_dead2.{ext}"},
    {"41efc3c84bb321af2b6b", "d_shawn2.{ext}"},
    {"41efc3c84bb321af2b6b", "d_shawn3.{ext}"},
    {"19237754d2eb85f41d84", "d_count2.{ext}"},
    {"a55d400570ad255a576b", "d_messg2.{ext}"},
    {"1736c81aac77f9bffd3d", "d_romer2.{ext}"},

    // These tracks are reused in Alien Vendetta, but are MIDs:
    {"9433604c098b7b1119a4", "d_in_cit.{ext}"},  // MAP26

    // Heretic tracks.
    {"12818ca0d3c957e7d57e", "mus_titl.{ext}"},
    {"5cb988538ce1b1857349", "mus_intr.{ext}"},
    {"6f126abe35a78b61b930", "mus_cptd.{ext}"},
    {"62557250f0427c067dc9", "mus_e1m1.{ext}"},
    {"1e8d5fd814490b9ae166", "mus_e1m2.{ext}"},
    {"f0f31e8834e85035d434", "mus_e1m3.{ext}"},
    {"054d6997405cc5a32b46", "mus_e1m4.{ext}"},
    {"31950ab062cc1e5ca49d", "mus_e1m5.{ext}"},
    {"7389024fbab0dff47211", "mus_e1m6.{ext}"},
    {"f2aa312dddd0a294a095", "mus_e1m7.{ext}"},
    {"cd6856731d1ae1f3aa4e", "mus_e1m8.{ext}"},
    {"d7fe793f266733d92e61", "mus_e1m9.{ext}"},
    {"933545b48fad8c66f042", "mus_e2m1.{ext}"},
    {"bf88ecd4ae1621222592", "mus_e2m2.{ext}"},
    {"4f619f87a828c2ca4801", "mus_e2m3.{ext}"},
    {"13033a83c49424b2f2ab", "mus_e2m4.{ext}"},
    {"b3851f9351ae411d9de3", "mus_e2m6.{ext}"},
    {"82539791159fbbc02a23", "mus_e2m7.{ext}"},
    {"fd9e53a49cfa62c463a0", "mus_e2m8.{ext}"},
    {"29503959324d2ca67958", "mus_e2m9.{ext}"},
    {"3aa632257c5be375b97b", "mus_e3m2.{ext}"},
    {"69ba0dce7913d53b67a8", "mus_e3m3.{ext}"},

    // These Heretic tracks are reused in Alien Vendetta, but are MIDs:
    {"51344131e8d260753ce7", "mus_e2m3.{ext}"},  // MAP15
    {"78b570b2397570440aff", "mus_e1m1.{ext}"},  // MAP19
    {"ee21ba9fad4de3dfaef0", "mus_e1m4.{ext}"},  // MAP29
    {"d2bb643a60696ccbca03", "mus_e1m9.{ext}"},  // MAP32

    // Hexen tracks:
    {"fbf55fc1ee26bd01266b", "winnowr.{ext}"},
    {"71776e2da2b7ba607d81", "jachr.{ext}"},
    {"c5c8630608b8132b33cd", "simonr.{ext}"},
    {"43683b3f55a031de88d4", "wutzitr.{ext}"},
    {"a6062883f29436ef73db", "falconr.{ext}"},
    {"512cb6cc9b558d5f0fef", "levelr.{ext}"},
    {"d31226ae75fce6a24208", "chartr.{ext}"},
    {"bf1f1e561bbdba4e699f", "swampr.{ext}"},
    {"b303193f756ca0e2de0f", "deepr.{ext}"},
    {"f0635f0386d883b00186", "fubasr.{ext}"},
    {"18f2a01f83df6e3abedc", "grover.{ext}"},
    {"b2527eb0522f08b2cf5f", "fortr.{ext}"},
    {"343addba8ba53a20a160", "foojar.{ext}"},
    {"c13109045b06b5a63386", "sixater.{ext}"},
    {"693525aaf69eac5429ab", "wobabyr.{ext}"},
    {"8f884223811c2bb8311d", "cryptr.{ext}"},
    {"de540e6826e62b32c01c", "fantar.{ext}"},
    {"efdff548df918934f71f", "blechr.{ext}"},
    {"de91f150f6a127e72e35", "voidr.{ext}"},
    {"e0497fe27289fe18515b", "chap_1r.{ext}"},
    {"f2ef1abdc3f672a3519a", "chap_2r.{ext}"},
    {"78cd9882f61cc441bef4", "chap_3r.{ext}"},
    {"97b2b575d9d096c1f89f", "chap_4r.{ext}"},
    {"ad0197a0f6c52ac30915", "chippyr.{ext}"},
    {"30506c62e9f0989ffe09", "percr.{ext}"},
    {"3542803beaa43bf1de1a", "secretr.{ext}"},
    {"81067721f40c611d09fb", "bonesr.{ext}"},
    {"4822af2e1a2eb7faf660", "octor.{ext}"},
    {"26bb3cec902ed8008fc2", "rithmr.{ext}"},
    {"94ab641c7aa93caac77a", "stalkr.{ext}"},
    {"d0a3f337c54b0703b4d3", "borkr.{ext}"},
    {"79e7781ec7eb9b9434b5", "crucibr.{ext}"},
    {"c2786e5581a7f8801969", "hexen.{ext}"},
    {"97fae9a084c0efda5151", "hub.{ext}"},
    {"c5da52d5c2ec4803ef8f", "hall.{ext}"},
    {"1e71bc0e2feafb06214e", "orb.{ext}"},
    {"bc9dcfa6632e847e03af", "chess.{ext}"},

    // Hexen CD tracks: alternate filenames for a ripped copy of
    // the CD soundtrack.
    {"71776e2da2b7ba607d81", "hexen02.{ext}"},   // level  2 (jachr)
    {"efdff548df918934f71f", "hexen03.{ext}"},   // level 26 (blechr)
    {"c2786e5581a7f8801969", "hexen04.{ext}"},   // (hexen)
    {"1e71bc0e2feafb06214e", "hexen05.{ext}"},   // (orb)
    {"f0635f0386d883b00186", "hexen06.{ext}"},   // level 10 (fubasr)
    {"bc9dcfa6632e847e03af", "hexen07.{ext}"},   // (chess)
    {"8f884223811c2bb8311d", "hexen08.{ext}"},   // level 24 (cryptr)
    {"a6062883f29436ef73db", "hexen09.{ext}"},   // level  5 (falconr)
    {"4822af2e1a2eb7faf660", "hexen10.{ext}"},   // level 36 (octor)
    {"26bb3cec902ed8008fc2", "hexen11.{ext}"},   // level 37 (rithmr)
    {"c13109045b06b5a63386", "hexen12.{ext}"},   // level 22 (sixater)
    {"fbf55fc1ee26bd01266b", "hexen13.{ext}"},   // level  1 (winnowr)
    {"bf1f1e561bbdba4e699f", "hexen14.{ext}"},   // level  8 (swampr)
    {"43683b3f55a031de88d4", "hexen15.{ext}"},   // level  4 (wutzitr)
    {"81067721f40c611d09fb", "hexen16.{ext}"},   // level 35 (bonesr)
    {"e0497fe27289fe18515b", "hexen17.{ext}"},   // level 28 (chap_1r)
    {"97b2b575d9d096c1f89f", "hexen18.{ext}"},   // level 31 (chap_4r)
    {"de540e6826e62b32c01c", "hexen19.{ext}"},   // level 25 (fantar)
    {"343addba8ba53a20a160", "hexen20.{ext}"},   // level 21 (foojar)
    {"512cb6cc9b558d5f0fef", "hexen21.{ext}"},   // level  6 (levelr)
    {"c5c8630608b8132b33cd", "hexen22.{ext}"},   // level  3 (simonr)

    // Strife:
    {"8ac2b2b47707f0fdf8f6", "d_logo.{ext}"},   // Title
    {"62e1c58054a1f1bc39b2", "d_action.{ext}"}, // 1,15,28
    {"12fa000f3fa1edac5c4f", "d_tavern.{ext}"}, // 2
    {"695e56ab3251792d20e5", "d_danger.{ext}"}, // 3,11
    {"96fe30e8712217b60dd7", "d_fast.{ext}"},   // 4
    {"61345598a3de04aad508", "d_darker.{ext}"}, // 6,14
    {"52353e9a435b7b1cb268", "d_strike.{ext}"}, // 7,19
    {"061164504907bffc9c22", "d_slide.{ext}"},  // 8,18,22
    {"3dbb4b703ce69aafcdd5", "d_tribal.{ext}"}, // 9
    {"393773688eba050c3548", "d_march.{ext}"},  // 10
    {"3cba3c627de065a667dd", "d_mood.{ext}"},   // 12
    {"b1f65a333e5c70255784", "d_castle.{ext}"}, // 13
    {"e1455a83a04c9ac4a09f", "d_fight.{ext}"},  // 16,31
    {"17f822b7374b1f069b89", "d_spense.{ext}"}, // 17
    {"e66c5a1a7d05f021f4ae", "d_dark.{ext}"},   // 20
    {"1c92bd0625026af30dad", "d_tech.{ext}"},   // 21,27
    {"7ae280713d078de7933a", "d_drone.{ext}"},  // 23,30
    {"4a664afd0d7eae79c97a", "d_panthr.{ext}"}, // 24
    {"4a7d62beeac5601ccf21", "d_sad.{ext}"},    // 25
    {"e60e109779400f2855d7", "d_instry.{ext}"}, // 26,29
    {"b7d36878faeb291d6df5", "d_happy.{ext}"},  // Better ending
    {"ff4a342c8c5ec51b06c3", "d_end.{ext}"},    // Worse ending
    // This conflicts with Doom's d_intro:
    //{"ec8fa484c4e85adbf700", "d_intro.{ext}"},  // 5

    // Sigil tracks
    {"31e9932f8260b1e8ebd5e110af8749d46aa95365", "d_e5m1.{ext}"},
    {"7211fc6f2f48cf1f27022690da6ed9b6363cb671", "d_e5m2.{ext}"},
    {"f7fe0a11e9a5fe6aecdf6a36299df33dd201eaef", "d_e5m3.{ext}"},
    {"fdcbb17ba408bc90af2bc295a6f6e51dd650d247", "d_e5m4.{ext}"},
    {"1d8a8dd6b374287b8848f2d65d4f6e0900b835e3", "d_e5m5.{ext}"},
    {"921f7ac3bcb97e44f2c48b1ae79310848587d47b", "d_e5m6.{ext}"},
    {"e5b6631a1a657518371f61794cb7ff801ea57a68", "d_e5m7.{ext}"},
    {"f86a6143fa0d5e38a2e09e011428bc867e3fb9a8", "d_e5m8.{ext}"},
    {"f86a11a12f525210b973c52583841cea1fc8ec79", "d_e5m9.{ext}"},
    {"bfac802f3fbbf9008ba0e74bfae46e5c687becb3", "d_sigint.{ext}"}, // D_INTER
    {"b7f165797cd500b351612b73267313f1df20a41e", "d_sigtit.{ext}"}, // D_INTRO
};

typedef struct
{
    char *hash_prefix;      // lowercase hex, 1..40 digits
    char *path;             // full path, as it is on the disk
} subst_t;

static subst_t *subst;
static int      subst_len;
static int      subst_cap;

static char     dirs[MAX_DIRS][256];
static int      num_dirs;
static boolean  initialized;

static boolean IsDir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static boolean IsFile(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

// dir/name with name matched without case, component by component
// ("Doom/E1M1.ogg"). Returns the path as it is on the disk, or false.
static boolean FindNoCase(const char *dir, const char *relpath,
                          char *out, size_t outlen)
{
    char cur[512], part[256];
    const char *p = relpath;

    M_StringCopy(cur, dir, sizeof(cur));

    while (*p != '\0')
    {
        const char *slash;
        size_t n;
        DIR *d;
        struct dirent *e;
        boolean found = false;

        while (*p == '/' || *p == '\\')
        {
            p++;
        }
        slash = p;
        while (*slash != '\0' && *slash != '/' && *slash != '\\')
        {
            slash++;
        }
        n = slash - p;
        if (n == 0 || n >= sizeof(part))
        {
            break;
        }
        memcpy(part, p, n);
        part[n] = '\0';
        p = slash;

        d = opendir(cur);
        if (d == NULL)
        {
            return false;
        }
        while ((e = readdir(d)) != NULL)
        {
            if (strcasecmp(e->d_name, part) == 0)
            {
                size_t len = strlen(cur);

                M_snprintf(cur + len, sizeof(cur) - len, "%s%s",
                           (len > 0 && cur[len - 1] == '/') ? "" : "/",
                           e->d_name);
                found = true;
                break;
            }
        }
        closedir(d);

        if (!found)
        {
            return false;
        }
    }

    if (!IsFile(cur))
    {
        return false;
    }

    M_StringCopy(out, cur, outlen);
    return true;
}

// The subfolder name of dir, matched without case ("Music", "MUSIC").
static boolean IsAudioName(const char *name, boolean *flac)
{
    size_t n = strlen(name);

    *flac = n > 5 && strcasecmp(name + n - 5, ".flac") == 0;
    return *flac || (n > 4 && (strcasecmp(name + n - 4, ".ogg") == 0
                            || strcasecmp(name + n - 4, ".mp3") == 0));
}

// A pack whose files carry the song's name among other words
// ("D_E1M1 - At Doom's Gate.ogg", "01 Hangar (e1m1).mp3"): a file with a
// word (letters and digits) equal to the song's name without its d_ /
// mus_ prefix. Only names with a digit or of 5 letters or more ("e1m1",
// "runnin"), so "doom" doesn't take any file of a Doom pack. Ogg and MP3
// before FLAC.
static boolean MatchWord(const char *dir, const char *bare,
                         char *out, size_t outlen)
{
    size_t blen = strlen(bare);
    boolean digit = false;
    int pass;
    size_t i;

    for (i = 0; i < blen; i++)
    {
        if (isdigit((unsigned char) bare[i]))
        {
            digit = true;
        }
        else if (!isalnum((unsigned char) bare[i]))
        {
            return false;   // the_da and such: no single word
        }
    }
    if (blen == 0 || (!digit && blen < 5))
    {
        return false;
    }

    for (pass = 0; pass < 2; pass++)
    {
        DIR *d = opendir(dir);
        struct dirent *e;

        if (d == NULL)
        {
            return false;
        }
        while ((e = readdir(d)) != NULL)
        {
            const char *p = e->d_name, *end;
            boolean flac;

            if (!IsAudioName(e->d_name, &flac) || flac != (pass == 1))
            {
                continue;
            }
            end = strrchr(e->d_name, '.');
            while (p < end)
            {
                const char *w;

                while (p < end && !isalnum((unsigned char) *p))
                {
                    p++;
                }
                w = p;
                while (p < end && isalnum((unsigned char) *p))
                {
                    p++;
                }
                if ((size_t) (p - w) == blen && strncasecmp(w, bare, blen) == 0)
                {
                    M_snprintf(out, outlen, "%s/%s", dir, e->d_name);
                    closedir(d);
                    return true;
                }
            }
        }
        closedir(d);
    }

    return false;
}

// A file name of a pack: ".{ext}" tries .ogg then .mp3; a .flac (not
// decoded here) tries the same name in .ogg and .mp3.
static boolean ResolveName(const char *dir, const char *name,
                           char *out, size_t outlen)
{
    // the lighter ones first: a FLAC goes whole into memory
    static const char *exts[] = { ".ogg", ".mp3", ".flac" };
    char base[256];
    size_t len = strlen(name);
    int i;

    if (len >= sizeof(base))
    {
        return false;
    }

    M_StringCopy(base, name, sizeof(base));

    if (len > 6 && strcasecmp(base + len - 6, ".{ext}") == 0)
    {
        base[len - 6] = '\0';
    }
    else if (len > 5 && strcasecmp(base + len - 5, ".flac") == 0)
    {
        base[len - 5] = '\0';
    }
    else
    {
        return FindNoCase(dir, name, out, outlen);
    }

    for (i = 0; i < (int) arrlen(exts); i++)
    {
        char try[256];

        M_snprintf(try, sizeof(try), "%s%s", base, exts[i]);
        if (FindNoCase(dir, try, out, outlen))
        {
            return true;
        }
    }

    // Some packs drop the lump prefix: e1m1.ogg for D_E1M1, e1m1.ogg for
    // Heretic's MUS_E1M1 (each game looks in its own folder first).
    {
        const char *bare = NULL;

        if (strncasecmp(base, "d_", 2) == 0)
        {
            bare = base + 2;
        }
        else if (strncasecmp(base, "mus_", 4) == 0)
        {
            bare = base + 4;
        }
        for (i = 0; bare != NULL && *bare != '\0' && i < (int) arrlen(exts); i++)
        {
            char try[256];

            M_snprintf(try, sizeof(try), "%s%s", bare, exts[i]);
            if (FindNoCase(dir, try, out, outlen))
            {
                return true;
            }
        }
        if (MatchWord(dir, bare != NULL ? bare : base, out, outlen))
        {
            return true;
        }
    }

    return false;
}

static void AddSubst(const char *hash_prefix, const char *path)
{
    if (subst_len == subst_cap)
    {
        int cap = subst_cap ? subst_cap * 2 : 64;
        subst_t *n = realloc(subst, cap * sizeof(*n));

        if (n == NULL)
        {
            return;
        }
        subst = n;
        subst_cap = cap;
    }

    subst[subst_len].hash_prefix = M_StringDuplicate(hash_prefix);
    subst[subst_len].path = M_StringDuplicate(path);
    subst_len++;
}

// "<hex> = <file>  # comment", as Chocolate Doom reads it.
static void ParseLine(const char *dir, char *line)
{
    char hash[41], path[512];
    char *p, *q;
    int n = 0;

    if ((p = strchr(line, '#')) != NULL)
    {
        *p = '\0';
    }
    for (p = line; isspace((unsigned char) *p); p++);
    if (*p == '\0')
    {
        return;
    }

    while (isxdigit((unsigned char) *p) && n < 40)
    {
        hash[n++] = tolower((unsigned char) *p++);
    }
    hash[n] = '\0';
    if (n == 0)
    {
        return;
    }

    while (isspace((unsigned char) *p))
    {
        p++;
    }
    if (*p++ != '=')
    {
        return;
    }
    while (isspace((unsigned char) *p))
    {
        p++;
    }
    q = p + strlen(p);
    while (q > p && isspace((unsigned char) q[-1]))
    {
        *--q = '\0';
    }
    if (*p == '\0')
    {
        return;
    }

    if (ResolveName(dir, p, path, sizeof(path)))
    {
        AddSubst(hash, path);
    }
}

static int ReadConfig(const char *dir, const char *file)
{
    char path[512], line[1024];
    FILE *f;
    int before = subst_len;

    M_snprintf(path, sizeof(path), "%s/%s", dir, file);
    f = fopen(path, "r");
    if (f == NULL)
    {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        ParseLine(dir, line);
    }
    fclose(f);

    PS3_Log("[musicpack] %s: %d song(s) found", path, subst_len - before);
    return subst_len - before;
}

static void ScanDir(const char *dir, int depth)
{
    DIR *d;
    struct dirent *e;
    int i, before, audio = 0;
    char sample[3][64];

    if (!IsDir(dir) || num_dirs >= MAX_DIRS)
    {
        return;
    }
    M_StringCopy(dirs[num_dirs++], dir, sizeof(dirs[0]));

    // What is in it, for the log.
    d = opendir(dir);
    if (d != NULL)
    {
        while ((e = readdir(d)) != NULL)
        {
            boolean flac;

            if (IsAudioName(e->d_name, &flac))
            {
                if (audio < 3)
                {
                    M_StringCopy(sample[audio], e->d_name, sizeof(sample[0]));
                }
                audio++;
            }
        }
        closedir(d);
    }
    PS3_Log("[musicpack] %s: %d audio file(s)%s%s%s%s%s%s", dir, audio,
            audio > 0 ? ", e.g. " : "", audio > 0 ? sample[0] : "",
            audio > 1 ? ", " : "", audio > 1 ? sample[1] : "",
            audio > 2 ? ", " : "", audio > 2 ? sample[2] : "");
    // Each folder in it is a pack of its own (doom1/, doom2/...), two
    // levels down (a zip's pack/music/ folder).
    if (depth < 2)
    {
        d = opendir(dir);
        if (d != NULL)
        {
            char sub[512];

            while ((e = readdir(d)) != NULL)
            {
                if (e->d_name[0] == '.')
                {
                    continue;
                }
                M_snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
                if (IsDir(sub))
                {
                    ScanDir(sub, depth + 1);
                }
            }
            closedir(d);
        }
    }

    // The pack's own configs first: they win over the file names.
    d = opendir(dir);
    if (d != NULL)
    {
        while ((e = readdir(d)) != NULL)
        {
            size_t n = strlen(e->d_name);

            if (n > 4 && strcasecmp(e->d_name + n - 4, ".cfg") == 0)
            {
                ReadConfig(dir, e->d_name);
            }
        }
        closedir(d);
    }

    before = subst_len;
    for (i = 0; i < (int) arrlen(known_filenames); i++)
    {
        char path[512];

        if (ResolveName(dir, known_filenames[i].filename, path, sizeof(path)))
        {
            AddSubst(known_filenames[i].hash_prefix, path);
        }
    }
    if (subst_len > before)
    {
        PS3_Log("[musicpack] %s: %d song(s) by file name", dir,
                subst_len - before);
    }
}

void PS3MusicPack_Init(void)
{
    if (initialized)
    {
        return;
    }
    initialized = true;

    // USRDIR/music-packs of this game (made by the PKG), then
    // /dev_hdd0/data/crispycell/music for the four games: the packs, each
    // in a folder of its own (doom1/, doom2/, heretic/...) or straight in.
    // Each song is looked for in all of them; one no pack has stays MIDI.
    ScanDir(PACK_ROOT, 0);
    ScanDir(PACK_SHARED, 0);

    if (num_dirs == 0)
    {
        PS3_Log("[musicpack] no %s: MIDI music", PACK_ROOT);
    }
    else
    {
        PS3_Log("[musicpack] %d song(s) in the music packs", subst_len);
    }
}

// Songs the music packs have (0 = no pack: not offered in the menu).
int PS3MusicPack_Count(void)
{
    if (!initialized)
    {
        PS3MusicPack_Init();
    }
    return subst_len;
}

// The lump the engine cached at this address, to try its name.
static const char *LumpNameOf(const void *data)
{
    static char name[9];
    unsigned int i;

    for (i = 0; i < numlumps; i++)
    {
        if (lumpinfo[i]->cache == data)
        {
            M_StringCopy(name, lumpinfo[i]->name, sizeof(name));
            return name;
        }
    }

    return NULL;
}

// The pack's file for this song, or NULL for the MIDI.
const char *PS3MusicPack_Find(const void *data, int len)
{
    static char found[512];
    sha1_context_t ctx;
    sha1_digest_t hash;
    char hex[sizeof(sha1_digest_t) * 2 + 1];
    const char *name;
    int i;

    if (!initialized)
    {
        PS3MusicPack_Init();
    }
    if (num_dirs == 0 || data == NULL || len <= 0)
    {
        return NULL;
    }

    SHA1_Init(&ctx);
    SHA1_Update(&ctx, (byte *) data, len);
    SHA1_Final(hash, &ctx);
    for (i = 0; i < (int) sizeof(sha1_digest_t); i++)
    {
        M_snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", hash[i]);
    }

    for (i = 0; i < subst_len; i++)
    {
        size_t n = strlen(subst[i].hash_prefix);

        if (strncmp(hex, subst[i].hash_prefix, n) == 0)
        {
            M_StringCopy(found, subst[i].path, sizeof(found));
            return found;
        }
    }

    // A song no table knows (new music in a PWAD): by the lump's name.
    name = LumpNameOf(data);
    if (name != NULL)
    {
        char file[16];

        M_snprintf(file, sizeof(file), "%s.{ext}", name);
        for (i = 0; i < num_dirs; i++)
        {
            if (ResolveName(dirs[i], file, found, sizeof(found)))
            {
                return found;
            }
        }
    }

    PS3_Log("[musicpack] no file for the song %s (sha1 %.20s)",
            name ? name : "?", hex);
    return NULL;
}
