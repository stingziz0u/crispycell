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
//      Timer functions -- PS3 native, via sysGetSystemTime()
//      (microseconds since an arbitrary epoch), same primitive
//      TyrQuakeCell's Sys_DoubleTime uses.
//

#include <sys/systime.h>

#include "i_timer.h"
#include "m_fixed.h" // [crispy]
#include "doomtype.h"

static uint64_t basecounter = 0;

static uint64_t GetUS(void)
{
    uint64_t now = sysGetSystemTime();

    if (basecounter == 0)
    {
        basecounter = now;
    }

    return now - basecounter;
}

//
// I_GetTime
// returns time in 1/35th second tics
//

int I_GetTime(void)
{
    return (int)((GetUS() / 1000) * TICRATE / 1000);
}

//
// Same as I_GetTime, but returns time in milliseconds
//

int I_GetTimeMS(void)
{
    return (int)(GetUS() / 1000);
}

// [crispy] Get time in microseconds

uint64_t I_GetTimeUS(void)
{
    return GetUS();
}

// Sleep for a specified number of ms

void I_Sleep(int ms)
{
    sysUsleep((u32)ms * 1000);
}

void I_WaitVBL(int count)
{
    I_Sleep((count * 1000) / 70);
}

void I_InitTimer(void)
{
    // sysGetSystemTime() needs no init; basecounter lazily latches on
    // first read.
}

// [crispy]

fixed_t I_GetFracRealTime(void)
{
    return (int64_t)I_GetTimeMS() * TICRATE % 1000 * FRACUNIT / 1000;
}
