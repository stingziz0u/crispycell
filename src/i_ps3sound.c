//
// i_ps3sound.c -- PS3 native SFX backend for Crispy Doom.
//
// Doom's SFX lumps (DS*) are 8-bit unsigned mono PCM, usually 11025 Hz.
// This module decodes them to 16-bit signed mono at the audio port's
// rate, caches the result on sfxinfo->driver_data, and mixes the active
// channels into the float block that opl_ps3.c's audio thread is about
// to hand to the hardware.
//
// There is only ONE libaudio port. audioSetNotifyEventQueue() is global,
// not per-port, so a second port would have both threads stealing each
// other's DMA notifications. opl_ps3.c owns the port and calls
// PS3Sound_MixInto() once per block. This mirrors upstream, where
// opl_sdl.c registers as an SDL_mixer postmix hook and music and SFX
// share a single device.
//

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mutex.h>

#include "deh_str.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_swap.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "doomtype.h"

extern void PS3_Log(const char *fmt, ...);

#define PS3_SFX_CHANNELS  32
#define PS3_SFX_RATE      48000

typedef struct
{
    int16_t      *samples;
    unsigned int  num_samples;
} ps3_sfx_t;

typedef struct
{
    const int16_t *samples;
    unsigned int   num_samples;
    unsigned int   pos;
    int            left;
    int            right;
    int            playing;
} ps3_channel_t;

static ps3_channel_t sfx_channels[PS3_SFX_CHANNELS];
static sys_mutex_t   sfx_mutex;
static boolean       sound_initialized = false;
static boolean       use_sfx_prefix;

// ---------------------------------------------------------------------
// Lump decoding
// ---------------------------------------------------------------------

// Read one sample from the source lump as signed 16-bit. The lump is
// little-endian regardless of host, so 16-bit reads are assembled from
// bytes by hand instead of casting.
static int GetSrcSample(const byte *data, unsigned int bits, unsigned int i)
{
    if (bits == 8)
    {
        return ((int) data[i] - 128) << 8;
    }
    else
    {
        int v = (int) data[i * 2] | ((int) data[i * 2 + 1] << 8);
        if (v >= 32768)
        {
            v -= 65536;
        }
        return v;
    }
}

static boolean CacheSFX(sfxinfo_t *sfxinfo)
{
    int lumpnum;
    unsigned int lumplen;
    int samplerate;
    unsigned int bits;
    unsigned int length;
    unsigned int out_samples;
    unsigned int i;
    uint32_t step;
    uint32_t p;
    byte *lump;
    byte *data;
    int16_t *out;
    ps3_sfx_t *sfx;

    lumpnum = sfxinfo->lumpnum;

    if (lumpnum < 0)
    {
        return false;
    }

    lump = W_CacheLumpNum(lumpnum, PU_STATIC);
    lumplen = W_LumpLength(lumpnum);
    data = lump;

    if (lumplen > 44 && memcmp(data, "RIFF", 4) == 0
     && memcmp(data + 8, "WAVEfmt ", 8) == 0)
    {
        // RIFF WAV lump (PWADs sometimes use these)
        int check;

        check = data[16] | (data[17] << 8) | (data[18] << 16) | (data[19] << 24);
        if (check != 16) goto reject;

        check = data[20] | (data[21] << 8);
        if (check != 1) goto reject;          // PCM only

        check = data[22] | (data[23] << 8);
        if (check != 1) goto reject;          // mono only

        samplerate = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
        length = data[40] | (data[41] << 8) | (data[42] << 16) | (data[43] << 24);

        if (length > lumplen - 44)
        {
            length = lumplen - 44;
        }

        bits = data[34] | (data[35] << 8);
        if (bits != 8 && bits != 16) goto reject;

        data += 44;
        if (bits == 16)
        {
            length /= 2;
        }
    }
    else if (lumplen >= 8 && data[0] == 0x03 && data[1] == 0x00)
    {
        // Standard Doom sound lump
        samplerate = (data[3] << 8) | data[2];
        length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];

        // DMX discards very short lumps and lumps whose header lies.
        if (length > lumplen - 8 || length <= 48) goto reject;

        bits = 8;

        // DMX skips the first and last 16 bytes of the lump.
        data += 8 + 16;
        length -= 32;
    }
    else
    {
        goto reject;
    }

    if (samplerate <= 0 || length == 0)
    {
        goto reject;
    }

    // Resample to the port rate with linear interpolation. 16.16 fixed
    // point: floats here would be fine too, but this keeps the whole
    // conversion in integer maths.
    out_samples = (unsigned int)
        (((uint64_t) length * PS3_SFX_RATE) / (unsigned int) samplerate);

    if (out_samples == 0)
    {
        goto reject;
    }

    out = malloc(out_samples * sizeof(int16_t));
    if (out == NULL)
    {
        goto reject;
    }

    step = (uint32_t) (((uint64_t) samplerate << 16) / PS3_SFX_RATE);
    p = 0;

    for (i = 0; i < out_samples; i++)
    {
        unsigned int idx = p >> 16;
        int frac = (int) (p & 0xffff);
        int s0, s1;

        if (idx >= length)
        {
            idx = length - 1;
        }

        s0 = GetSrcSample(data, bits, idx);
        s1 = (idx + 1 < length) ? GetSrcSample(data, bits, idx + 1) : s0;

        out[i] = (int16_t) (s0 + (((s1 - s0) * frac) >> 16));
        p += step;
    }

    sfx = malloc(sizeof(ps3_sfx_t));
    if (sfx == NULL)
    {
        free(out);
        goto reject;
    }

    sfx->samples = out;
    sfx->num_samples = out_samples;
    sfxinfo->driver_data = sfx;

    W_ReleaseLumpNum(lumpnum);
    return true;

reject:
    W_ReleaseLumpNum(lumpnum);
    return false;
}

// ---------------------------------------------------------------------
// Mixer -- runs on opl_ps3.c's audio thread
// ---------------------------------------------------------------------

void PS3Sound_MixInto(float *dst, unsigned int frames, int ch)
{
    unsigned int i;
    unsigned int total;

    if (!sound_initialized)
    {
        return;
    }

    sysMutexLock(sfx_mutex, 0);

    for (i = 0; i < PS3_SFX_CHANNELS; i++)
    {
        ps3_channel_t *c = &sfx_channels[i];
        unsigned int n;
        unsigned int j;
        float lg, rg;

        if (!c->playing || c->samples == NULL)
        {
            continue;
        }

        n = frames;
        if (c->pos + n > c->num_samples)
        {
            n = c->num_samples - c->pos;
        }

        lg = (float) c->left / (255.0f * 32768.0f);
        rg = (float) c->right / (255.0f * 32768.0f);

        for (j = 0; j < n; j++)
        {
            float s = (float) c->samples[c->pos + j];
            dst[j * ch] += s * lg;
            if (ch > 1)
            {
                dst[j * ch + 1] += s * rg;
            }
        }

        c->pos += n;

        if (c->pos >= c->num_samples)
        {
            c->playing = 0;
        }
    }

    sysMutexUnlock(sfx_mutex);

    // Music and SFX are summed into the same block, so clip rather than
    // let the float wrap into garbage at the hardware.
    total = frames * (unsigned int) ch;
    for (i = 0; i < total; i++)
    {
        if (dst[i] > 1.0f)
        {
            dst[i] = 1.0f;
        }
        else if (dst[i] < -1.0f)
        {
            dst[i] = -1.0f;
        }
    }
}

// ---------------------------------------------------------------------
// sound_module_t implementation
// ---------------------------------------------------------------------

static void I_PS3_UpdateSoundParams(int channel, int vol, int sep)
{
    int left, right;

    if (!sound_initialized || channel < 0 || channel >= PS3_SFX_CHANNELS)
    {
        return;
    }

    left  = ((254 - sep) * vol) / 127;
    right = ((sep) * vol) / 127;

    if (left < 0) left = 0;
    else if (left > 255) left = 255;
    if (right < 0) right = 0;
    else if (right > 255) right = 255;

    sysMutexLock(sfx_mutex, 0);
    sfx_channels[channel].left = left;
    sfx_channels[channel].right = right;
    sysMutexUnlock(sfx_mutex);
}

static int I_PS3_StartSound(sfxinfo_t *sfxinfo, int channel,
                            int vol, int sep, int pitch)
{
    ps3_sfx_t *sfx;

    if (!sound_initialized || channel < 0 || channel >= PS3_SFX_CHANNELS)
    {
        return -1;
    }

    if (sfxinfo->driver_data == NULL && !CacheSFX(sfxinfo))
    {
        return -1;
    }

    sfx = (ps3_sfx_t *) sfxinfo->driver_data;

    sysMutexLock(sfx_mutex, 0);
    sfx_channels[channel].samples = sfx->samples;
    sfx_channels[channel].num_samples = sfx->num_samples;
    sfx_channels[channel].pos = 0;
    sfx_channels[channel].playing = 1;
    sysMutexUnlock(sfx_mutex);

    I_PS3_UpdateSoundParams(channel, vol, sep);

    return channel;
}

static void I_PS3_StopSound(int channel)
{
    if (!sound_initialized || channel < 0 || channel >= PS3_SFX_CHANNELS)
    {
        return;
    }

    sysMutexLock(sfx_mutex, 0);
    sfx_channels[channel].playing = 0;
    sysMutexUnlock(sfx_mutex);
}

static boolean I_PS3_SoundIsPlaying(int channel)
{
    boolean result;

    if (!sound_initialized || channel < 0 || channel >= PS3_SFX_CHANNELS)
    {
        return false;
    }

    sysMutexLock(sfx_mutex, 0);
    result = sfx_channels[channel].playing != 0;
    sysMutexUnlock(sfx_mutex);

    return result;
}

static void I_PS3_UpdateSound(void)
{
    // The mixer retires finished channels itself; nothing to poll.
}

static int I_PS3_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }

    if (use_sfx_prefix)
    {
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(namebuf, DEH_String(sfx->name), sizeof(namebuf));
    }

    // [crispy] missing sounds are non-fatal
    return W_CheckNumForName(namebuf);
}

static void I_PS3_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    // Lumps are decoded lazily on first play. Doom's sounds are small
    // and the PS3 has the memory, so the startup cost isn't worth it.
}

static boolean I_PS3_InitSound(GameMission_t mission)
{
    sys_mutex_attr_t attr;
    int i;

    use_sfx_prefix = (mission == doom || mission == strife);

    for (i = 0; i < PS3_SFX_CHANNELS; i++)
    {
        memset(&sfx_channels[i], 0, sizeof(ps3_channel_t));
    }

    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&sfx_mutex, &attr) != 0)
    {
        PS3_Log("I_PS3_InitSound: sysMutexCreate failed");
        return false;
    }

    sound_initialized = true;
    PS3_Log("I_PS3_InitSound: OK, %d channels @ %d Hz, prefix=%d",
            PS3_SFX_CHANNELS, PS3_SFX_RATE, (int) use_sfx_prefix);

    return true;
}

static void I_PS3_ShutdownSound(void)
{
    if (!sound_initialized)
    {
        return;
    }

    // Deliberately NOT destroying sfx_mutex here. S_Shutdown calls
    // I_ShutdownSound() before I_ShutdownMusic(), so at this point the
    // OPL audio thread is still alive and still calling
    // PS3Sound_MixInto(), which locks this mutex. Destroying it now
    // means that thread locks freed handle -- and a fault inside the
    // audio thread during shutdown takes the console down with it.
    // Clearing the flag is enough: MixInto returns early, and the
    // process is about to exit anyway, so leaking one mutex costs
    // nothing.
    sound_initialized = false;
}

static const snddevice_t sound_ps3_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

const sound_module_t sound_ps3_module =
{
    sound_ps3_devices,
    arrlen(sound_ps3_devices),
    I_PS3_InitSound,
    I_PS3_ShutdownSound,
    I_PS3_GetSfxLumpNum,
    I_PS3_UpdateSound,
    I_PS3_UpdateSoundParams,
    I_PS3_StartSound,
    I_PS3_StopSound,
    I_PS3_SoundIsPlaying,
    I_PS3_PrecacheSounds,
};
