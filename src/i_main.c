//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
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
//	Main program, simply calls D_DoomMain high level loop.
//

#include "config.h"
#include "crispy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h> // [crispy] setlocale

#ifdef PS3_BUILD
#include <sys/thread.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>
#include <dirent.h>
#include <errno.h>

// Declares the .sys_proc_param ELF section -- the PS3 loader reads this
// to size the primary thread's stack and set its priority before any of
// our code runs. ECellWolf's binary has this section and boots;
// ours didn't have it at all, which is the one structural difference
// left between this binary and the two known-good ones.
SYS_PROCESS_PARAM(1001, 0x100000)
#else
#include "SDL.h"
#endif

#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"


//
// D_DoomMain()
// Not a globally visible function, just included for source reference,
// calls all startup code, parses command line options.
//

void D_DoomMain (void);

#ifdef PS3_BUILD

// Same pattern as TyrQuakeCell's sys_ps3.c PS3_QuakeThread/main: the PS3
// OS gives the default EBOOT thread a small fixed stack, so the real
// work runs on a worker thread created with a real stack size, and
// main() just launches it and joins.

#define PS3_DOOM_STACK (2 * 1024 * 1024)
#define PS3_DOOM_PRIO  1000

static int g_argc;
static char **g_argv;

extern void PS3_Log(const char *fmt, ...);
extern void PS3_LogV(const char *fmt, ...);
extern boolean PS3_Launcher_SelectIWAD(char *out_path, size_t out_size);
extern int PS3_Launcher_GetAddons(const char **paths, int max);

// Fires when the PS button opens the XMB overlay or "Quit Game" is
// picked from it. Registered below, but only ever delivered when
// something calls sysUtilCheckCallback() -- i_ps3video.c does that once
// per frame from I_FinishUpdate. Without it the process is killed
// outright while RSX still owns the display buffers and the audio port
// is still open, which takes the whole console down, not just us.
// I_Quit() runs every I_AtExit handler (config save, sound, RSX) and
// then exits, which is exactly the clean path we want.
static void PS3_SysutilCallback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;

    switch (status)
    {
    case SYSUTIL_EXIT_GAME:
        PS3_Log("PS3_SysutilCallback: SYSUTIL_EXIT_GAME -- clean shutdown");
        I_Quit();
        break;
    default:
        PS3_LogV("PS3_SysutilCallback: status=0x%llx (not EXIT_GAME)",
                (unsigned long long) status);
        break;
    }
}

static void PS3_DoomThread(void *arg)
{
    PS3_Log("=== Crispy Doom PS3 boot (worker thread) ===");

    if (sysUtilRegisterCallback(0, PS3_SysutilCallback, NULL) != 0)
    {
        PS3_Log("WARNING: sysUtilRegisterCallback failed -- PS button exit "
                "will not be clean");
    }
    else
    {
        PS3_Log("sysUtilRegisterCallback OK");
    }
    // Inject -iwad explicitly with a fixed path -- no shell/FTP client
    // on PS3 to pass real command-line args, and D_FindIWAD's directory
    // search (env vars, exedir/cwd guesses) is uncertain territory on
    // this platform. This makes the WAD location unambiguous: FTP the
    // file to exactly this path and it's found, no guessing needed.
    // Pick the IWAD before D_DoomMain, which is where -iwad gets
    // consumed. The launcher shows a menu only when there is more than
    // one IWAD present; with a single one it returns straight away.
    static char ps3_iwad_buf[320];
    const char *ps3_iwad_path;

    if (PS3_Launcher_SelectIWAD(ps3_iwad_buf, sizeof(ps3_iwad_buf)))
    {
        ps3_iwad_path = ps3_iwad_buf;
    }
    else
    {
        // Nothing found. Hand the engine a plausible name so its own
        // error path reports something meaningful in the log.
#if defined(PS3_GAME_HERETIC)
        ps3_iwad_path = PS3_USRDIR "/HERETIC.WAD";
#elif defined(PS3_GAME_HEXEN)
        ps3_iwad_path = PS3_USRDIR "/HEXEN.WAD";
#elif defined(PS3_GAME_STRIFE)
        ps3_iwad_path = PS3_USRDIR "/STRIFE1.WAD";
#else
        ps3_iwad_path = PS3_USRDIR "/DOOM1.WAD";
#endif
        PS3_Log("No IWAD found, falling back to '%s'", ps3_iwad_path);
    }

    // -iwad <game> [-file <addon>...]. Doom's -file takes any number
    // of filenames after it, so the add-ons the launcher returned all
    // hang off a single -file.
    #define PS3_MAX_ADDONS 16
    const char *ps3_addons[PS3_MAX_ADDONS];
    int ps3_num_addons = PS3_Launcher_GetAddons(ps3_addons, PS3_MAX_ADDONS);
    int ps3_argi;

    // -noautoload is always on: the launcher decides what loads, and
    // Crispy's own auto-load would otherwise pull SIGIL/NERVE/MASTER
    // LEVELS in behind our back, since every WAD sits in one directory
    // here. See the comment at the top of i_ps3launcher.c.
    myargc = g_argc + 3 + (ps3_num_addons > 0 ? 1 + ps3_num_addons : 0);
    myargv = malloc(myargc * sizeof(char *));
    assert(myargv != NULL);

    for (int i = 0; i < g_argc; i++)
    {
        myargv[i] = M_StringDuplicate(g_argv[i]);
    }

    ps3_argi = g_argc;
    myargv[ps3_argi++] = M_StringDuplicate("-noautoload");
    myargv[ps3_argi++] = M_StringDuplicate("-iwad");
    myargv[ps3_argi++] = M_StringDuplicate(ps3_iwad_path);

    if (ps3_num_addons > 0)
    {
        int a;

        myargv[ps3_argi++] = M_StringDuplicate("-file");

        for (a = 0; a < ps3_num_addons; a++)
        {
            myargv[ps3_argi++] = M_StringDuplicate(ps3_addons[a]);
        }
    }

    setlocale(LC_TIME, "");

    if (M_ParmExists("-version") || M_ParmExists("--version")) {
        puts(PACKAGE_STRING);
        sysThreadExit(0);
    }

    crispy->sdlversion = M_StringDuplicate("n/a (PS3 native)");
    crispy->platform = "PS3";

    M_FindResponseFile();
    M_SetExeDir();

    PS3_Log("Calling D_DoomMain...");
    D_DoomMain ();

    PS3_Log("D_DoomMain returned (unexpected, should never exit this way)");
    sysThreadExit(0);
}

int main(int argc, char **argv)
{
    sys_ppu_thread_t thread_id;
    u64 exit_code = 0;
    s32 rc;

    PS3_Log("=== EBOOT main() entered, argc=%d ===", argc);

    g_argc = argc;
    g_argv = argv;

    rc = sysThreadCreate(&thread_id, PS3_DoomThread, NULL,
                          PS3_DOOM_PRIO, PS3_DOOM_STACK,
                          THREAD_JOINABLE, (char *)"crispy_doom_game");
    PS3_Log("main: sysThreadCreate rc=%d", (int)rc);
    if (rc != 0)
    {
        PS3_Log("main: sysThreadCreate FAILED, bailing out");
        return 1;
    }

    PS3_Log("main: waiting on sysThreadJoin...");
    sysThreadJoin(thread_id, &exit_code);
    PS3_Log("main: sysThreadJoin returned, exit_code=%llu", (unsigned long long)exit_code);
    return (int)exit_code;
}

#else

int main(int argc, char **argv)
{
    // save arguments

    myargc = argc;
    myargv = malloc(argc * sizeof(char *));
    assert(myargv != NULL);

    for (int i = 0; i < argc; i++)
    {
        myargv[i] = M_StringDuplicate(argv[i]);
    }

    // [crispy] Print date and time in the Load/Save Game menus in the current locale
    setlocale(LC_TIME, "");

    //!
    // Print the program version and exit.
    //
    if (M_ParmExists("-version") || M_ParmExists("--version")) {
        puts(PACKAGE_STRING);
        exit(0);
    }

    {
        char buf[16];
        SDL_version version;
        SDL_GetVersion(&version);
        M_snprintf(buf, sizeof(buf), "%d.%d.%d", version.major, version.minor, version.patch);
        crispy->sdlversion = M_StringDuplicate(buf);
        crispy->platform = SDL_GetPlatform();
    }

#if defined(_WIN32)
    // compose a proper command line from loose file paths passed as arguments
    // to allow for loading WADs and DEHACKED patches by drag-and-drop
    M_AddLooseFiles();
#endif

    M_FindResponseFile();
    M_SetExeDir();

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    // start doom

    D_DoomMain ();

    return 0;
}

#endif // PS3_BUILD

