//
// i_ps3debug.h -- [PS3] crash diagnostics of the debug package
// (./ps3/build.sh strife-debug, PS3_DEBUG_CHEATS). Everything here
// compiles to nothing in the release packages.
//

#ifndef I_PS3DEBUG_H
#define I_PS3DEBUG_H

#ifdef PS3_DEBUG_CHEATS

void PS3_Crumb(const char *tag, int val);
void PS3_Debug_ThreadStart(void);
void PS3_Debug_Frame(void);
void PS3_Debug_Dump(const char *why);
extern void PS3_Log(const char *fmt, ...);

// A breadcrumb in memory (no I/O): the last 128 are written to the log
// if the game thread dies.
#define PS3_CRUMB(tag, val)  PS3_Crumb((tag), (int) (val))
// A rare event, straight to the log.
#define PS3_DEBUG_LOG(...)   PS3_Log(__VA_ARGS__)

#else

#define PS3_CRUMB(tag, val)  ((void) 0)
#define PS3_DEBUG_LOG(...)   ((void) 0)

#endif

#endif
