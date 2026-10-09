//
// i_ps3debug.c -- [PS3] crash diagnostics of the debug package.
//
// There is no way to get the PC of a crash on a retail console (lv2's
// exception handler is not there, see the re3-PS3 notes), but when the
// game thread dies the main thread lives on, waiting in sysThreadJoin.
// So the game thread leaves a trail in memory, with no I/O, and the main
// thread writes it to the log when the join returns with an error:
//
// - crumbs: the last 128 {tag, value, gametic, caller} from PS3_CRUMB.
// - ftrace: the debug package compiles the Strife code (all but the
//   column/span drawers) with -finstrument-functions; every function
//   entry and exit of the game thread goes into a ring of 65536 events.
//   The dump rebuilds from it the calls still open when the thread died
//   -- the stack at the crash, innermost last -- and adds the last 256
//   raw events. The addresses are function descriptors (.opd):
//   ps3/tools/crashaddr.py names them from the package's ELF.
//
// Plus a heartbeat every 30 s: frames per second, map, position, zone.
//

#include <stdio.h>
#include <stdint.h>

#include "doomtype.h"
#include "d_loop.h"
#include "i_timer.h"
#include "z_zone.h"
#include "i_ps3debug.h"

#ifdef PS3_DEBUG_CHEATS

#define NO_TRACE __attribute__((no_instrument_function))

#define NCRUMBS     128
#define NFTRACE     65536       // power of two
#define GAME_STACK  (2 * 1024 * 1024)   // i_main.c PS3_DOOM_STACK

#define PS3_LOG_PATH_A PS3_USRDIR "/crispy_log.txt"

typedef struct
{
    const char *tag;
    int         val;
    int         tic;
    void       *where;
} crumb_t;

static crumb_t      crumbs[NCRUMBS];
static unsigned     crumb_n;

static unsigned     ftrace[NFTRACE];
static unsigned     ftrace_n;
static uintptr_t    stack_lo, stack_hi;     // the game thread's stack

static int          frames, beat_time, beat_frames;
static int          dumped;

void NO_TRACE __attribute__((noinline)) PS3_Crumb(const char *tag, int val)
{
    crumb_t *c = &crumbs[crumb_n++ % NCRUMBS];

    c->tag = tag;
    c->val = val;
    c->tic = gametic;
    c->where = __builtin_return_address(0);
}

// Called first thing in the game thread: only its calls are traced (the
// audio thread runs some of the same code).
void NO_TRACE PS3_Debug_ThreadStart(void)
{
    char here;

    stack_hi = (uintptr_t) &here + 4096;
    stack_lo = stack_hi - GAME_STACK;
    PS3_Log("[debug] debug package: crumbs and function trace on");
}

static inline int NO_TRACE OnGameThread(void)
{
    uintptr_t sp = (uintptr_t) __builtin_frame_address(0);

    return sp > stack_lo && sp < stack_hi;
}

void NO_TRACE __cyg_profile_func_enter(void *fn, void *site);
void NO_TRACE __cyg_profile_func_exit(void *fn, void *site);

void NO_TRACE __cyg_profile_func_enter(void *fn, void *site)
{
    (void) site;
    if (OnGameThread())
    {
        ftrace[ftrace_n++ & (NFTRACE - 1)] = (unsigned) (uintptr_t) fn;
    }
}

void NO_TRACE __cyg_profile_func_exit(void *fn, void *site)
{
    (void) site;
    if (OnGameThread())
    {
        ftrace[ftrace_n++ & (NFTRACE - 1)] = (unsigned) (uintptr_t) fn | 1;
    }
}

// Once a frame (D_Display): the heartbeat.
void NO_TRACE PS3_Debug_Frame(void)
{
    int now = I_GetTimeMS();

    frames++;
    if (beat_time == 0)
    {
        beat_time = now;
        beat_frames = frames;
        return;
    }
    if (now - beat_time >= 30000)
    {
        int f = frames - beat_frames;
        int ms = now - beat_time;

        PS3_Log("[hb] tic %d: %d.%d fps over %d s, zone free %d KB",
                gametic, f * 1000 / ms, (f * 10000 / ms) % 10, ms / 1000,
                Z_FreeMemory() / 1024);
        beat_time = now;
        beat_frames = frames;
    }
}

// Writes the trail to the log: from the main thread after the game
// thread died, or from I_Error. One fopen for all of it.
void NO_TRACE PS3_Debug_Dump(const char *why)
{
    static unsigned stack[256];
    FILE *f;
    unsigned n, first, i, depth = 0, lost = 0;

    if (dumped++)
    {
        return;
    }

    f = fopen(PS3_LOG_PATH_A, "a");
    if (f == NULL)
    {
        return;
    }

    fprintf(f, "[crash] %s, gametic %d, %u crumbs, %u trace events\n",
            why, gametic, crumb_n, ftrace_n);

    // crumbs, oldest first
    n = crumb_n < NCRUMBS ? crumb_n : NCRUMBS;
    for (i = crumb_n - n; i != crumb_n; i++)
    {
        const crumb_t *c = &crumbs[i % NCRUMBS];

        fprintf(f, "[crumb] tic %d %s %d @%08x\n", c->tic,
                c->tag ? c->tag : "?", c->val,
                (unsigned) (uintptr_t) c->where);
    }

    // the calls still open at the end of the ring
    n = ftrace_n < NFTRACE ? ftrace_n : NFTRACE;
    first = ftrace_n - n;
    for (i = first; i != ftrace_n; i++)
    {
        unsigned e = ftrace[i & (NFTRACE - 1)];

        if ((e & 1) == 0)
        {
            if (depth < 256)
            {
                stack[depth++] = e;
            }
            else
            {
                lost++;     // deeper than we keep
            }
        }
        else
        {
            unsigned fn = e & ~1u;
            int k;

            // back to where it was entered; an exit whose entry is
            // older than the ring is a caller that was already running
            for (k = (int) depth - 1; k >= 0 && stack[k] != fn; k--)
            {
            }
            if (k >= 0)
            {
                depth = (unsigned) k;
            }
            else
            {
                lost++;
            }
        }
    }
    fprintf(f, "[stack] %u calls open at the end (outermost first), "
               "%u returns into callers older than the trace\n", depth, lost);
    for (i = 0; i < depth; i++)
    {
        fprintf(f, "[stack] %u %08x\n", i, stack[i]);
    }

    // the last raw events, 8 a line: odd = exit
    n = ftrace_n < 256 ? ftrace_n : 256;
    for (i = 0; i < n; i++)
    {
        if (i % 8 == 0)
        {
            fprintf(f, "%s[ftrace]", i ? "\n" : "");
        }
        fprintf(f, " %08x", ftrace[(ftrace_n - n + i) & (NFTRACE - 1)]);
    }
    if (n > 0)
    {
        fprintf(f, "\n");
    }

    fprintf(f, "[crash] end\n");
    fclose(f);
}

#endif  // PS3_DEBUG_CHEATS
