//
// i_videohr.c -- PS3 stub for Hexen's VGA-emulation loading screen.
//
// Returning false from I_SetVideoModeHR makes Hexen skip this whole
// special-mode sequence entirely (see hexen/st_start.c) and fall
// straight through to normal startup -- no separate video mode to
// tear down, nothing else here ever gets called.
//

#include "doomtype.h"
#include "i_videohr.h"

boolean I_SetVideoModeHR(void)
{
    return false;
}

void I_UnsetVideoModeHR(void)
{
}

void I_SetWindowTitleHR(const char *title)
{
}

void I_ClearScreenHR(void)
{
}

void I_SlamBlockHR(int x, int y, int w, int h, const byte *src)
{
}

void I_SlamHR(const byte *buffer)
{
}

void I_InitPaletteHR(void)
{
}

void I_SetPaletteHR(const byte *palette)
{
}

void I_FadeToPaletteHR(const byte *palette)
{
}

void I_BlackPaletteHR(void)
{
}

boolean I_CheckAbortHR(void)
{
    return false;
}
