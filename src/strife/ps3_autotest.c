//
// ps3_autotest.c -- [PS3] Strife plays itself, for chasing crashes.
//
// Debug package only (./ps3/build.sh strife-debug, PS3_DEBUG_CHEATS).
// Started and stopped with L3 + R3 + SELECT inside a level. While it
// runs, the real pad is ignored (but for that chord) and this file feeds
// i_ps3joystick.c a made-up pad instead, so everything goes through the
// same input code a player's presses do: menus, dialogues, the automap,
// the inventory, weapons.
//
// For every map, in order, and round again after the last one, until it
// is stopped:
//   setup      god mode, all weapons and keys, medical items, backpack,
//              upgrades
//   walk       30 s of wandering: sticks, fire, use, jump, weapon changes
//   automap    open, zoom in and out, move, overlay and rotate on/off,
//              open and close it quickly a few times
//   inventory  scroll, use items, medkit, objectives and keys pop-ups
//   save/load  saves to the last slot ("AUTOTEST") and loads it back
//   next       rift to the next map that exists
// A dialogue or any menu that opens on the way is answered with Cross
// (first choice) a few times, then Circle until it closes.
//
// Every step goes to the log as "[auto] ...", and a heartbeat every 10 s
// says where the player is: after a crash the last lines say what it was
// doing. The log file is opened and closed for every line, so nothing
// written before a crash is lost.
//

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"
#include "m_menu.h"
#include "m_misc.h"
#include "m_controls.h"
#include "p_inter.h"
#include "p_dialog.h"
#include "p_saveg.h"
#include "i_timer.h"
#include "am_map.h"
#include "crispy.h"

#ifdef PS3_DEBUG_CHEATS

extern void PS3_Log(const char *fmt, ...);
extern void M_DoSave(int slot);
extern void M_ReadSaveStrings(void);
extern char savegamestrings[10][SAVESTRINGSIZE];
void ST_PS3DebugCheat(int which);   // st_stuff.c

#define SEC(n)      ((n) * TICRATE)
#define AUTO_SLOT   5           // the last of the six save slots
#define VB_CROSS    (1u << 0)   // menu confirm (i_ps3joystick.c VBTN_*)
#define VB_CIRCLE   (1u << 2)   // menu back

typedef enum
{
    PH_SETUP,
    PH_WALK,
    PH_AUTOMAP,
    PH_INVENTORY,
    PH_SAVE,
    PH_LOAD,
    PH_NEXT,
    PH_WAITMAP,
} phase_t;

static const char *const phase_names[] =
{
    "setup", "walk", "automap", "inventory", "save", "load", "next",
    "waiting for the map",
};

static boolean  active;
static phase_t  phase;
static int      phase_start;        // I_GetTime() when the phase began
static int      pass, maps_done;
static unsigned seed;
static int      next_beat;
static int      wait_from_map;

// walk
static int      walk_until;
static int      walk_lx, walk_ly, walk_rx, walk_ry;
static unsigned walk_held;

// menus and dialogues
static int      menu_since = -1;
static int      menu_presses;

// automap
static int      am_last_tap = -100;
static int      am_old_overlay, am_old_rotate;
static boolean  am_settings_changed;

// demo screen after the game ended
static int      demo_since = -1;

static unsigned Rand(void)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7fff;
}

static int RandRange(int lo, int hi)
{
    return lo + (int) (Rand() % (unsigned) (hi - lo + 1));
}

// The virtual button a binding is on (0 if unbound).
static unsigned B(int joyb)
{
    return (joyb >= 0 && joyb < 16) ? 1u << joyb : 0;
}

// A short press at tic t of the phase: down for 3 tics.
static boolean Tap(int t, int at)
{
    return t >= at && t < at + 3;
}

extern void PS3_Crumb(const char *tag, int val);

static void SetPhase(phase_t p)
{
    phase = p;
    phase_start = I_GetTime();
    PS3_Crumb("autotest phase", (int) p | (gamemap << 8));
    PS3_Log("[auto] pass %d map %d: %s", pass, gamemap, phase_names[p]);
}

static void Beat(void)
{
    player_t *p = &players[consoleplayer];

    PS3_Log("[auto] pass %d map %d %s t=%ds state %d pos %d,%d hp %d "
            "weapon %d inv %d/%d automap %d menu %d",
            pass, gamemap, phase_names[phase],
            (I_GetTime() - phase_start) / TICRATE, (int) gamestate,
            p->mo ? p->mo->x >> FRACBITS : 0,
            p->mo ? p->mo->y >> FRACBITS : 0,
            p->health, (int) p->readyweapon,
            p->inventorycursor, p->numinventory,
            (int) automapactive, (int) menuactive);
}

static void RestoreAutomapSettings(void)
{
    if (am_settings_changed)
    {
        crispy->automapoverlay = am_old_overlay;
        crispy->automaprotate = am_old_rotate;
        am_settings_changed = false;
    }
}

void PS3_Autotest_Toggle(void)
{
    if (active)
    {
        active = false;
        RestoreAutomapSettings();
        PS3_Log("[auto] stopped: pass %d, %d maps done", pass, maps_done);
        return;
    }

    if (gamestate != GS_LEVEL || netgame || !usergame)
    {
        PS3_Log("[auto] start it from inside a level");
        return;
    }

    active = true;
    pass = 1;
    maps_done = 0;
    seed = 7919u;
    next_beat = I_GetTime() + SEC(10);
    menu_since = -1;
    demo_since = -1;
    PS3_Log("[auto] start on map %d: widescreen %d hires %d smoothmap %d "
            "overlay %d rotate %d uncapped %d", gamemap,
            crispy->widescreen, crispy->hires, crispy->smoothmap,
            crispy->automapoverlay, crispy->automaprotate,
            crispy->uncapped);
    SetPhase(PH_SETUP);
}

static void GiveEverything(void)
{
    player_t *p = &players[consoleplayer];
    int i;

    if (!(p->cheats & CF_GODMODE))
    {
        ST_PS3DebugCheat(0);
    }
    ST_PS3DebugCheat(1);    // weapons, ammo, armor
    ST_PS3DebugCheat(3);    // keys

    // inventory to scroll through and use (pumpuph / pumpupp / pumpups)
    P_GiveItemToPlayer(p, SPR_STMP, MT_INV_MED1);
    P_GiveItemToPlayer(p, SPR_MDKT, MT_INV_MED2);
    P_GiveItemToPlayer(p, SPR_FULL, MT_INV_MED3);
    if (!p->backpack)
    {
        for (i = 0; i < NUMAMMO; i++)
        {
            p->maxammo[i] *= 2;
        }
        p->backpack = true;
    }
    for (i = 0; i < NUMAMMO; i++)
    {
        p->ammo[i] = p->maxammo[i];
    }
    P_GiveItemToPlayer(p, SPR_TOKN, MT_TOKEN_STAMINA);
    P_GiveItemToPlayer(p, SPR_TOKN, MT_TOKEN_NEW_ACCURACY);
    P_GivePower(p, pw_targeter);
}

static unsigned Walk(int t, int *lx, int *ly, int *rx, int *ry)
{
    unsigned vb;
    int now = I_GetTime();

    if (t == 0 || now >= walk_until)
    {
        walk_until = now + RandRange(20, 80);
        walk_lx = RandRange(-127, 127);
        walk_ly = RandRange(-127, 60);      // mostly forward
        walk_rx = RandRange(-70, 70);
        walk_ry = (Rand() % 4 == 0) ? RandRange(-50, 50) : 0;
        walk_held = 0;
        if (Rand() % 2)
        {
            walk_held |= B(joybfire);
        }
        if (Rand() % 3 == 0)
        {
            walk_held |= B(joybspeed);
        }
    }

    *lx = walk_lx;
    *ly = walk_ly;
    *rx = walk_rx;
    *ry = walk_ry;
    vb = walk_held;

    if (t % 70 < 3)
    {
        vb |= B(joybuse);
    }
    if (t % 210 >= 100 && t % 210 < 103)
    {
        vb |= B(joybjump);
    }
    if (t % 175 >= 50 && t % 175 < 53)
    {
        vb |= B(joybnextweapon);
    }
    if (t % 315 >= 200 && t % 315 < 203)
    {
        vb |= B(joybprevweapon);
    }

    return vb;
}

// Automap: whether it should be open at tic t, and what is held.
static unsigned Automap(int t, int *ly, int *rx)
{
    static const struct { int at; boolean open; } want[] =
    {
        { 0, true }, { 310, false }, { 330, true }, { 350, false },
        { 370, true }, { 390, false }, { 410, true }, { 440, false },
    };
    boolean open = false;
    unsigned vb = 0;
    int i, now = I_GetTime();

    for (i = 0; i < (int) arrlen(want); i++)
    {
        if (t >= want[i].at)
        {
            open = want[i].open;
        }
    }

    if (automapactive != open && now - am_last_tap > 8)
    {
        am_last_tap = now;
        return B(joybautomap);
    }
    if (now - am_last_tap < 3)
    {
        return B(joybautomap);      // finish the tap
    }

    if (t >= 10 && t < 80)
    {
        vb |= B(joybmapzoomin);
    }
    else if (t >= 90 && t < 190)
    {
        vb |= B(joybmapzoomout);
    }
    else if (t >= 200 && t < 300)
    {
        *ly = -100;
        *rx = 40;
    }

    if (t == 150 && !am_settings_changed)
    {
        am_old_overlay = crispy->automapoverlay;
        am_old_rotate = crispy->automaprotate;
        am_settings_changed = true;
        crispy->automapoverlay = 1;
        crispy->automaprotate = 1;
        PS3_Log("[auto] automap overlay + rotate on");
    }
    if (t == 260)
    {
        RestoreAutomapSettings();
        PS3_Log("[auto] automap overlay + rotate back");
    }

    return vb;
}

static unsigned Inventory(int t)
{
    static const struct { int at, hold; int *joyb; } steps[] =
    {
        { 10, 3, &joybinvright },  { 30, 3, &joybinvright },
        { 50, 3, &joybinvright },  { 70, 3, &joybuseartifact },
        { 100, 3, &joybinvleft },  { 120, 3, &joybinvleft },
        { 140, 3, &joybusehealth }, { 170, 50, &joybmission },
        { 240, 3, &joybmission },  { 260, 50, &joybinvkey },
        { 330, 3, &joybinvkey },   { 350, 3, &joybinvkey },
        { 380, 3, &joybuseartifact },
    };
    unsigned vb = 0;
    int i;

    for (i = 0; i < (int) arrlen(steps); i++)
    {
        if (t >= steps[i].at && t < steps[i].at + steps[i].hold)
        {
            vb |= B(*steps[i].joyb);
        }
    }

    return vb;
}

// Called by i_ps3joystick.c for every pad read. Returns false when not
// running (the real pad is used); otherwise fills in the virtual buttons
// (VBTN_* bits) and the sticks (-128..127).
boolean PS3_Autotest_Drive(unsigned *vbuttons, int *lx, int *ly,
                           int *rx, int *ry)
{
    player_t *p = &players[consoleplayer];
    int now, t;

    if (!active)
    {
        return false;
    }

    now = I_GetTime();
    t = now - phase_start;
    *vbuttons = 0;
    *lx = *ly = *rx = *ry = 0;

    if (now >= next_beat)
    {
        next_beat = now + SEC(10);
        Beat();
    }

    // The game ended (or never was): back on the title screen.
    if (gamestate == GS_DEMOSCREEN)
    {
        if (demo_since < 0)
        {
            demo_since = now;
            PS3_Log("[auto] on the title screen");
        }
        if (now - demo_since > SEC(10))
        {
            PS3_Log("[auto] stopped: the game is not running any more");
            active = false;
            RestoreAutomapSettings();
        }
        return true;
    }
    demo_since = -1;

    // Finale, intermission: press on.
    if (gamestate != GS_LEVEL)
    {
        *vbuttons = (now % 35 < 3) ? VB_CROSS : 0;
        return true;
    }

    // A dialogue or a menu: answer it, then back out of it.
    if (menuactive)
    {
        if (menu_since < 0)
        {
            menu_since = now;
            menu_presses = 0;
            PS3_Log("[auto] map %d: a menu or dialogue opened", gamemap);
        }
        if ((now - menu_since) % 25 == 0)
        {
            menu_presses++;
        }
        if ((now - menu_since) % 25 < 3)
        {
            *vbuttons = menu_presses <= 3 ? VB_CROSS : VB_CIRCLE;
        }
        if (now - menu_since > SEC(30))
        {
            PS3_Log("[auto] map %d: menu still open after 30 s, closing it",
                    gamemap);
            M_ClearMenus(0);
            menu_since = -1;
        }
        return true;
    }
    if (menu_since >= 0)
    {
        PS3_Log("[auto] map %d: menu closed after %d presses", gamemap,
                menu_presses);
        menu_since = -1;
    }

    // Dead in spite of god mode (a crusher, a script): respawn.
    if (p->playerstate == PST_DEAD)
    {
        *vbuttons = (now % 35 < 3) ? B(joybuse) : 0;
        return true;
    }

    switch (phase)
    {
        case PH_SETUP:
            if (t == 0)
            {
                GiveEverything();
            }
            if (t >= SEC(1))
            {
                SetPhase(PH_WALK);
            }
            break;

        case PH_WALK:
            *vbuttons = Walk(t, lx, ly, rx, ry);
            if (t >= SEC(30))
            {
                SetPhase(PH_AUTOMAP);
            }
            break;

        case PH_AUTOMAP:
            if (t < SEC(14))
            {
                *vbuttons = Automap(t, ly, rx);
            }
            else if (automapactive)
            {
                if (t < SEC(17))
                {
                    if (now - am_last_tap > 8)
                    {
                        am_last_tap = now;
                    }
                    *vbuttons = (now - am_last_tap < 3) ? B(joybautomap) : 0;
                }
                else
                {
                    PS3_Log("[auto] map %d: the automap would not close, "
                            "AM_Stop", gamemap);
                    AM_Stop();
                }
            }
            else
            {
                RestoreAutomapSettings();
                SetPhase(PH_INVENTORY);
            }
            break;

        case PH_INVENTORY:
            *vbuttons = Inventory(t);
            if (t >= SEC(12))
            {
                SetPhase(PH_SAVE);
            }
            break;

        case PH_SAVE:
            if (t == 0)
            {
                M_ReadSaveStrings();
                M_StringCopy(savegamestrings[AUTO_SLOT], "AUTOTEST",
                             SAVESTRINGSIZE);
                M_DoSave(AUTO_SLOT);
                PS3_Log("[auto] map %d: saved to slot %d", gamemap,
                        AUTO_SLOT + 1);
            }
            if (t >= SEC(4))
            {
                SetPhase(PH_LOAD);
            }
            break;

        case PH_LOAD:
            if (t == 0)
            {
                M_LoadSelect(AUTO_SLOT);
                PS3_Log("[auto] map %d: loading slot %d", gamemap,
                        AUTO_SLOT + 1);
            }
            if (t >= SEC(6))
            {
                SetPhase(PH_NEXT);
            }
            break;

        case PH_NEXT:
            wait_from_map = gamemap;
            ST_PS3DebugCheat(2);
            SetPhase(PH_WAITMAP);
            break;

        case PH_WAITMAP:
            if (gamemap != wait_from_map && t >= SEC(1))
            {
                maps_done++;
                if (gamemap < wait_from_map)
                {
                    PS3_Log("[auto] pass %d done (%d maps)", pass, maps_done);
                    pass++;
                    seed = 7919u * (unsigned) pass;
                }
                SetPhase(PH_SETUP);
            }
            else if (t >= SEC(30))
            {
                PS3_Log("[auto] map %d: still here 30 s after the rift, "
                        "trying again", gamemap);
                SetPhase(PH_NEXT);
            }
            break;
    }

    return true;
}

#endif  // PS3_DEBUG_CHEATS
