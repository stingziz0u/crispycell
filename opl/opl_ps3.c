//
// opl_ps3.c -- PS3 native OPL music backend using PSL1GHT libaudio.
//
// The callback-queue/timer logic here is copied verbatim from opl_sdl.c --
// that part never depended on SDL. What changes: locking uses sys_mutex_t
// instead of SDL_mutex, and instead of registering into SDL_mixer as a
// postmix effect, this opens its own libaudio port and dedicated audio
// thread, same pattern as common/snd_ps3.c's SNDDMA_Init/PS3_AudioThread.
//

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sys/thread.h>
#include <sys/mutex.h>

#include "opl3.h"

#include "opl.h"
#include "opl_internal.h"

#include "opl_queue.h"

#define PS3_OPL_AUDIO_RATE 48000

typedef struct
{
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t expire_time;
} opl_timer_t;

static sys_mutex_t callback_mutex;
static sys_mutex_t callback_queue_mutex;

static opl_callback_queue_t *callback_queue;

static uint64_t current_time;

static int opl_ps3_paused;
static uint64_t pause_offset;

static opl3_chip opl_chip;
static int opl_opl3mode;

static int16_t *mix_buffer = NULL;

static int register_num = 0;

extern void PS3_Log(const char *fmt, ...);
static volatile unsigned long long ps3_blocks_written = 0;
static volatile int ps3_peak = 0;

// Defined in src/i_ps3sound.c. There is only one libaudio port (the
// notify event queue is global, not per-port), so SFX are summed into
// this block instead of opening their own. Mirrors opl_sdl.c's postmix
// hook upstream.
extern void PS3Sound_MixInto(float *dst, unsigned int frames, int ch);
static volatile unsigned long long ps3_regwrites = 0;
static volatile unsigned long long ps3_callbacks = 0;
static volatile unsigned long long ps3_zero_ns = 0;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

static unsigned int mixing_freq = PS3_OPL_AUDIO_RATE;
static unsigned int mixing_channels = 2;

// --- PS3 audio port state (same shape as common/snd_ps3.c) ---
static u32               audio_port = (u32)-1;
static audioPortConfig   audio_config;
static sys_event_queue_t audio_queue;
static sys_ipc_key_t     audio_queue_key;
static int               last_filled_buf;
static volatile int      audio_running = 0;
static sys_ppu_thread_t  audio_thread_id;

static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    sysMutexLock(callback_queue_mutex, 0);

    us = ((uint64_t) nsamples * OPL_SECOND) / mixing_freq;
    current_time += us;

    if (opl_ps3_paused)
    {
        pause_offset += us;
    }

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
        {
            break;
        }

        sysMutexUnlock(callback_queue_mutex);

        sysMutexLock(callback_mutex, 0);
        ps3_callbacks++;
        callback(callback_data);
        sysMutexUnlock(callback_mutex);

        sysMutexLock(callback_queue_mutex, 0);
    }

    sysMutexUnlock(callback_queue_mutex);
}

// Fills nsamples stereo frames (int16) via the OPL3 emulator.
static void FillBuffer(int16_t *buffer, unsigned int nsamples)
{
    assert(nsamples < mixing_freq);
    OPL3_GenerateStream(&opl_chip, buffer, nsamples);
}

// Pulled by the hardware DMA notification -- same pattern as
// PS3_AudioThread in common/snd_ps3.c, but the source is the OPL3
// emulator instead of Quake's sound ring buffer.
static void
OPL_PS3_AudioThread(void *arg)
{
    int num_blocks = (int)audio_config.numBlocks;
    int ch = (int)audio_config.channelCount;
    int block_frames = AUDIO_BLOCK_SAMPLES;
    const float scale = 1.0f / 32768.0f;

    while (audio_running) {
        sys_event_t event;
        s32 ret = sysEventQueueReceive(audio_queue, &event, 20 * 1000);
        if (ret != 0)
            continue;

        int filling = (last_filled_buf + 1) % num_blocks;
        last_filled_buf = filling;
        ps3_blocks_written++;
        if ((ps3_blocks_written % 188) == 0) {
            PS3_Log("OPL_PS3: blocks=%llu peak=%d regw=%llu cb=%llu zerons=%llu",
                    ps3_blocks_written, ps3_peak, ps3_regwrites,
                    ps3_callbacks, ps3_zero_ns);
            ps3_peak = 0;
        }

        float *dst = (float *)(uintptr_t)audio_config.audioDataStart;
        dst += filling * block_frames * ch;

        unsigned int filled = 0;

        while (filled < (unsigned int)block_frames)
        {
            uint64_t next_callback_time;
            uint64_t nsamples;

            sysMutexLock(callback_queue_mutex, 0);

            if (opl_ps3_paused || OPL_Queue_IsEmpty(callback_queue))
            {
                nsamples = block_frames - filled;
            }
            else
            {
                next_callback_time = OPL_Queue_Peek(callback_queue) + pause_offset;

                nsamples = (next_callback_time - current_time) * mixing_freq;
                nsamples = (nsamples + OPL_SECOND - 1) / OPL_SECOND;

                if (nsamples > (uint64_t)(block_frames - filled))
                {
                    nsamples = block_frames - filled;
                }
            }

            sysMutexUnlock(callback_queue_mutex);

            if (nsamples == 0) { ps3_zero_ns++; nsamples = 1; }
            FillBuffer(mix_buffer, nsamples);

            for (unsigned int i = 0; i < nsamples * (unsigned int)ch; i++)
            {
                int sv = mix_buffer[i];
                if (sv < 0) sv = -sv;
                if (sv > ps3_peak) ps3_peak = sv;
                dst[filled * ch + i] = (float)mix_buffer[i] * scale;
            }

            filled += (unsigned int)nsamples;

            AdvanceTime((unsigned int)nsamples);
        }

        PS3Sound_MixInto(dst, (unsigned int)block_frames, ch);
    }

    sysThreadExit(0);
}

extern void PS3_Log(const char *fmt, ...);

static int OPL_PS3_Init(unsigned int port_base)
{
    PS3_Log("OPL_PS3_Init: start");
    s32 ret = audioInit();
    if (ret != 0)
    {
        fprintf(stderr, "OPL PS3: audioInit failed (%d)\n", (int)ret);
        return 0;
    }

    audioPortParam params;
    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks   = AUDIO_BLOCK_8;
    params.attrib      = 0;
    params.level       = 1;

    ret = audioPortOpen(&params, &audio_port);
    if (ret != 0)
    {
        fprintf(stderr, "OPL PS3: audioPortOpen failed (%d)\n", (int)ret);
        audioQuit();
        return 0;
    }

    ret = audioGetPortConfig(audio_port, &audio_config);
    if (ret != 0)
    {
        fprintf(stderr, "OPL PS3: audioGetPortConfig failed (%d)\n", (int)ret);
        audioPortClose(audio_port);
        audioQuit();
        return 0;
    }

    ret = audioCreateNotifyEventQueue(&audio_queue, &audio_queue_key);
    if (ret != 0)
    {
        fprintf(stderr, "OPL PS3: audioCreateNotifyEventQueue failed (%d)\n", (int)ret);
        audioPortClose(audio_port);
        audioQuit();
        return 0;
    }

    ret = audioSetNotifyEventQueue(audio_queue_key);
    if (ret != 0)
    {
        fprintf(stderr, "OPL PS3: audioSetNotifyEventQueue failed (%d)\n", (int)ret);
        audioPortClose(audio_port);
        sysEventQueueDestroy(audio_queue, 0);
        audioQuit();
        return 0;
    }

    sysEventQueueDrain(audio_queue);

    ret = audioPortStart(audio_port);
    if (ret != 0)
    {
        fprintf(stderr, "OPL PS3: audioPortStart failed (%d)\n", (int)ret);
        audioRemoveNotifyEventQueue(audio_queue_key);
        audioPortClose(audio_port);
        sysEventQueueDestroy(audio_queue, 0);
        audioQuit();
        return 0;
    }

    last_filled_buf = (int)(audio_config.numBlocks - 1);

    mixing_freq = PS3_OPL_AUDIO_RATE;
    mixing_channels = (unsigned int)audio_config.channelCount;

    callback_queue = OPL_Queue_Create();
    current_time = 0;
    opl_ps3_paused = 0;
    pause_offset = 0;

    mix_buffer = malloc(mixing_freq * mixing_channels * sizeof(int16_t));
    if (!mix_buffer)
    {
        fprintf(stderr, "OPL PS3: failed to allocate mix buffer\n");
        audioPortStop(audio_port);
        audioRemoveNotifyEventQueue(audio_queue_key);
        audioPortClose(audio_port);
        sysEventQueueDestroy(audio_queue, 0);
        audioQuit();
        return 0;
    }

    OPL3_Reset(&opl_chip, mixing_freq);
    opl_opl3mode = 0;

    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    sysMutexCreate(&callback_mutex, &attr);
    sysMutexCreate(&callback_queue_mutex, &attr);

    PS3_Log("OPL_PS3_Init: audio port open, freq=%d ch=%d blocks=%d",
            mixing_freq, (int)mixing_channels, (int)audio_config.numBlocks);

    audio_running = 1;
    ret = sysThreadCreate(&audio_thread_id, OPL_PS3_AudioThread, NULL,
                           1000, 64 * 1024, THREAD_JOINABLE, (char *)"ps3_opl_audio");
    if (ret != 0)
    {
        fprintf(stderr, "OPL PS3: sysThreadCreate failed (%d)\n", (int)ret);
        audio_running = 0;
        return 0;
    }

    return 1;
}

static void OPL_PS3_Shutdown(void)
{
    if (audio_running)
    {
        u64 exit_code;

        audio_running = 0;
        sysThreadJoin(audio_thread_id, &exit_code);

        audioPortStop(audio_port);
        audioRemoveNotifyEventQueue(audio_queue_key);
        audioPortClose(audio_port);
        sysEventQueueDestroy(audio_queue, 0);
        audioQuit();
    }

    if (callback_queue != NULL)
    {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }

    free(mix_buffer);
    mix_buffer = NULL;

    sysMutexDestroy(callback_mutex);
    sysMutexDestroy(callback_queue_mutex);
}

static unsigned int OPL_PS3_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
    {
        return 0xff;
    }

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;
        result |= 0x40;
    }

    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;
        result |= 0x20;
    }

    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;

    if (timer->enabled)
    {
        tics = 0x100 - timer->value;
        timer->expire_time = current_time
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }

                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }

            break;

        case OPL_REG_NEW:
            opl_opl3mode = value & 0x01;

        default:
            ps3_regwrites++;
            OPL3_WriteRegBuffered(&opl_chip, reg_num, value);
            break;
    }
}

static void OPL_PS3_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_REGISTER_PORT_OPL3)
    {
        register_num = value | 0x100;
    }
    else if (port == OPL_DATA_PORT)
    {
        WriteRegister(register_num, value);
    }
}

static void OPL_PS3_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    sysMutexLock(callback_queue_mutex, 0);
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time - pause_offset + us);
    sysMutexUnlock(callback_queue_mutex);
}

static void OPL_PS3_ClearCallbacks(void)
{
    sysMutexLock(callback_queue_mutex, 0);
    OPL_Queue_Clear(callback_queue);
    sysMutexUnlock(callback_queue_mutex);
}

static void OPL_PS3_Lock(void)
{
    sysMutexLock(callback_mutex, 0);
}

static void OPL_PS3_Unlock(void)
{
    sysMutexUnlock(callback_mutex);
}

static void OPL_PS3_SetPaused(int paused)
{
    opl_ps3_paused = paused;
}

static void OPL_PS3_AdjustCallbacks(float factor)
{
    sysMutexLock(callback_queue_mutex, 0);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    sysMutexUnlock(callback_queue_mutex);
}

opl_driver_t opl_ps3_driver =
{
    "PS3",
    OPL_PS3_Init,
    OPL_PS3_Shutdown,
    OPL_PS3_PortRead,
    OPL_PS3_PortWrite,
    OPL_PS3_SetCallback,
    OPL_PS3_ClearCallbacks,
    OPL_PS3_Lock,
    OPL_PS3_Unlock,
    OPL_PS3_SetPaused,
    OPL_PS3_AdjustCallbacks,
};
