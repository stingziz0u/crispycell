//
// i_ps3music.c -- music on the PS3: the original OPL, or a SoundFont.
//
// The OPL module (i_oplmusic.c over opl/opl_ps3.c) owns the one audio
// port and its thread, which also mixes the sound effects in -- there is
// only one libaudio port, see opl_ps3.c. So it is always started, and
// this module wraps it: every song is registered with it and with the
// SoundFont player, and played by whichever synth is chosen.
//
// Crispness -> Audible -> Music: "OPL" (the original, default) or any
// .sf2 in /dev_hdd0/data/crispycell/, one folder for the four games.
// The packages ship no SoundFont: the player makes that folder and puts
// one in (GeneralUser GS is the one tested, see the README), so the PKGs
// stay small and nobody else's work is redistributed in them.
// The choice is saved as ps3_music_sf2 (the file name, empty for OPL).
// Changing it restarts the current song with the new synth.
//
// The SoundFont synth is TinySoundFont (tsf.h, MIT, with the big-endian
// fix from ROTT-PS3) and the MIDI comes from its TinyMidiLoader (tml.h,
// MIT, reads the file byte by byte so it is endian-safe): MUS lumps are
// converted with Crispy's own mus2mid first. The MIDI events are sent
// and the samples rendered in the audio thread (PS3Music_MixInto, called
// by opl_ps3.c), under one mutex with everything the game thread changes.
// Loading a SoundFont takes a few seconds and happens in the game
// thread; the synth pointer is swapped under that mutex. TinySoundFont
// keeps the samples as floats: about twice the file's size in memory.
//
// MP3 music lumps (SIGIL_SHREDS.WAD, SIGIL_II_MP3.WAD) are not MIDI: the
// OPL module and the SoundFont can't play them, and upstream needs
// SDL_mixer for them. They play here as recorded, whatever the Music
// setting says -- a soundtrack WAD is there to be heard. minimp3.h (CC0,
// as in re3-PS3) decodes them a frame at a time in the audio thread,
// straight from a copy of the lump, and the samples are resampled to the
// port's 48 kHz with linear interpolation.
//

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include <sys/mutex.h>

#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "memio.h"
#include "mus2mid.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TML_IMPLEMENTATION
#define TML_NO_STDIO
#include "tml.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

extern void PS3_Log(const char *fmt, ...);

#define SF2_DIR     "/dev_hdd0/data/crispycell"     // shared by the 4 games
#define SF2_RATE    48000       // the audio port's (opl_ps3.c)
#define SF2_VOICES  64
#define SF2_MAX     16          // SoundFonts offered in the menu
#define SF2_NAMELEN 64
#define MAX_SONGS   4

// ps3_music_sf2 in the cfg: the .sf2 in use, "" = OPL
char *ps3_music_sf2 = "";

static char sf2_names[SF2_MAX][SF2_NAMELEN];
static int  sf2_count;

static sys_mutex_t sf2_mutex;
static boolean     sf2_mutex_ready;

// Everything below is shared with the audio thread: under sf2_mutex.
static tsf *synth;                  // NULL = OPL
static char synth_name[SF2_NAMELEN];

static struct
{
    void        *handle;            // the OPL module's, as the engine sees it
    tml_message *midi;
} songs[MAX_SONGS];

static tml_message *song_first;     // the song the SoundFont plays
static tml_message *song_pos;       // next event
static double       song_ms;
static boolean      song_playing;
static boolean      song_looping;
static boolean      song_paused;

// What the engine asked to play last (for a restart on a synth change).
static void   *current_handle;
static boolean current_looping;
static boolean current_on;
static int     music_volume = 127;

// A registered MP3 song: its handle is this struct.
typedef struct
{
    byte *data;         // our copy: Hexen releases the lump right away
    int   len;
    int   start;        // the first frame, after an ID3v2 tag
} mp3_song_t;

static mp3_song_t *mp3_songs[MAX_SONGS];

// MP3 playback, audio thread (under sf2_mutex like the rest).
static mp3_song_t  *mp3_cur;        // NULL = no MP3 playing
static boolean      mp3_looping;
static mp3dec_t     mp3_dec;
static int          mp3_pos;        // byte offset of the next frame
static mp3d_sample_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int          mp3_pcm_frames; // decoded frames in mp3_pcm
static int          mp3_pcm_next;   // next one to use
static int          mp3_pcm_ch;     // 1 or 2
static unsigned int mp3_step;       // source frames per output frame, 16.16
static unsigned int mp3_frac;       // position between mp3_a and mp3_b, 16.16
static float        mp3_a[2], mp3_b[2];
static unsigned int mp3_out_frames; // mixed since the start, for the log
static boolean      mp3_ended;      // ran out of frames (not looping)

static void Lock(void)
{
    if (sf2_mutex_ready)
    {
        sysMutexLock(sf2_mutex, 0);
    }
}

static void Unlock(void)
{
    if (sf2_mutex_ready)
    {
        sysMutexUnlock(sf2_mutex);
    }
}

// ---------------------------------------------------------------------
// SoundFonts on disk
// ---------------------------------------------------------------------

static boolean IsSoundFont(const char *name)
{
    size_t n = strlen(name);

    return n > 4 && strcasecmp(name + n - 4, ".sf2") == 0;
}

static int CompareNames(const void *a, const void *b)
{
    return strcasecmp((const char *) a, (const char *) b);
}

static void ScanDir(const char *dir)
{
    DIR *d;
    struct dirent *e;
    int i;

    d = opendir(dir);
    if (d == NULL)
    {
        return;
    }

    while ((e = readdir(d)) != NULL && sf2_count < SF2_MAX)
    {
        boolean dup = false;

        if (!IsSoundFont(e->d_name) || strlen(e->d_name) >= SF2_NAMELEN)
        {
            continue;
        }

        for (i = 0; i < sf2_count; i++)
        {
            if (strcasecmp(sf2_names[i], e->d_name) == 0)
            {
                dup = true;
            }
        }

        if (!dup)
        {
            M_StringCopy(sf2_names[sf2_count++], e->d_name, SF2_NAMELEN);
        }
    }

    closedir(d);
}

static void ScanSoundFonts(void)
{
    sf2_count = 0;

    ScanDir(SF2_DIR);

    qsort(sf2_names, sf2_count, SF2_NAMELEN, CompareNames);
}

static tsf *LoadSoundFont(const char *name)
{
    char path[320];
    tsf *f;
    int ch;

    M_snprintf(path, sizeof(path), "%s/%s", SF2_DIR, name);
    PS3_Log("[music] loading %s", path);

    f = tsf_load_filename(path);
    if (f == NULL)
    {
        PS3_Log("[music] %s: not a SoundFont, or out of memory", name);
        return NULL;
    }

    tsf_set_output(f, TSF_STEREO_INTERLEAVED, SF2_RATE, 0.0f);

    // Everything allocated here, nothing in the audio thread: the voices
    // up front (none are added later), and all 16 channels (setting 15
    // first allocates 0..15 in one go). Same as ROTT-PS3.
    if (!tsf_set_max_voices(f, SF2_VOICES))
    {
        PS3_Log("[music] out of memory for the voices");
        tsf_close(f);
        return NULL;
    }

    for (ch = 15; ch >= 0; ch--)
    {
        tsf_channel_set_presetnumber(f, ch, 0, ch == 9);
    }

    tsf_set_volume(f, music_volume / 127.0f);

    PS3_Log("[music] %s: %d presets", name, tsf_get_presetcount(f));
    return f;
}

// ---------------------------------------------------------------------
// The MP3 player (audio thread; started from the game thread)
// ---------------------------------------------------------------------

// The next source frame into out[], decoding when the last one is used
// up. At the end of the song it starts over if looping -- once per call
// at most, so a lump with no playable frame can't loop for ever under
// the lock -- and returns false otherwise.
static boolean Mp3Pull(float out[2])
{
    boolean restarted = false;

    while (mp3_pcm_next >= mp3_pcm_frames)
    {
        mp3dec_frame_info_t info;
        int n = 0;

        if (mp3_pos < mp3_cur->len)
        {
            n = mp3dec_decode_frame(&mp3_dec, mp3_cur->data + mp3_pos,
                                    mp3_cur->len - mp3_pos, mp3_pcm, &info);
        }
        else
        {
            info.frame_bytes = 0;
        }

        if (info.frame_bytes == 0)
        {
            // nothing more in the lump
            if (!mp3_looping || restarted)
            {
                return false;
            }
            mp3dec_init(&mp3_dec);
            mp3_pos = mp3_cur->start;
            restarted = true;
            continue;
        }

        mp3_pos += info.frame_bytes;

        if (n > 0 && info.hz > 0)
        {
            // 0 samples = an ID3 tag or junk skipped: try the next frame
            mp3_pcm_frames = n;
            mp3_pcm_next = 0;
            mp3_pcm_ch = info.channels > 1 ? 2 : 1;
            mp3_step = (unsigned int) (((uint64_t) info.hz << 16) / SF2_RATE);
        }
    }

    if (mp3_pcm_ch == 2)
    {
        out[0] = mp3_pcm[mp3_pcm_next * 2] * (1.0f / 32768.0f);
        out[1] = mp3_pcm[mp3_pcm_next * 2 + 1] * (1.0f / 32768.0f);
    }
    else
    {
        out[0] = out[1] = mp3_pcm[mp3_pcm_next] * (1.0f / 32768.0f);
    }
    mp3_pcm_next++;

    return true;
}

static void Mp3Stop(void)
{
    mp3_cur = NULL;
}

// What the last MP3 song did, for the log (game thread).
static void Mp3LogPlayed(void)
{
    unsigned int frames;
    boolean ended;

    Lock();
    frames = mp3_out_frames;
    ended = mp3_ended;
    mp3_out_frames = 0;
    mp3_ended = false;
    Unlock();

    if (frames > 0 || ended)
    {
        PS3_Log("[music] MP3 played %u.%u s%s", frames / SF2_RATE,
                (frames % SF2_RATE) * 10 / SF2_RATE,
                ended ? ", to the end" : "");
    }
}

// Starts a song from the top (under the lock).
static void Mp3Start(mp3_song_t *song, boolean looping)
{
    mp3_cur = song;
    mp3_looping = looping;
    mp3dec_init(&mp3_dec);
    mp3_pos = song->start;
    mp3_pcm_frames = mp3_pcm_next = 0;
    mp3_frac = 0;
    mp3_out_frames = 0;
    mp3_ended = false;
    song_paused = false;

    if (!Mp3Pull(mp3_a) || !Mp3Pull(mp3_b))
    {
        mp3_cur = NULL;
    }
}

// Adds the MP3 to the block (under the lock).
static void Mp3MixInto(float *dst, unsigned int frames)
{
    const float gain = music_volume / 127.0f;
    unsigned int i;

    for (i = 0; i < frames && mp3_cur != NULL; i++)
    {
        float t = mp3_frac * (1.0f / 65536.0f);

        dst[i * 2]     += (mp3_a[0] + (mp3_b[0] - mp3_a[0]) * t) * gain;
        dst[i * 2 + 1] += (mp3_a[1] + (mp3_b[1] - mp3_a[1]) * t) * gain;

        mp3_frac += mp3_step;
        while (mp3_frac >= 0x10000 && mp3_cur != NULL)
        {
            mp3_frac -= 0x10000;
            mp3_a[0] = mp3_b[0];
            mp3_a[1] = mp3_b[1];
            if (!Mp3Pull(mp3_b))
            {
                mp3_cur = NULL;     // the end, not looping
                mp3_ended = true;
            }
        }
    }
    mp3_out_frames += i;
}

// The size of the ID3v2 tag at the start, 0 if there is none. Skipping
// it keeps minimp3 from looking for frames in the cover art.
static int Id3Size(const byte *data, int len)
{
    int size;

    if (len < 10 || memcmp(data, "ID3", 3) != 0)
    {
        return 0;
    }

    size = ((data[6] & 0x7f) << 21) | ((data[7] & 0x7f) << 14)
         | ((data[8] & 0x7f) << 7) | (data[9] & 0x7f);
    size += 10;
    if (data[5] & 0x10)
    {
        size += 10;     // footer
    }

    return size < len ? size : 0;
}

// An MP3 file: minimp3 finds a run of layer III frames in it, after the
// ID3v2 tag if there is one. Not only at the very start: some files have
// padding or a stray tag before the first frame. MIDI and MUS never get
// here (PS3Music_RegisterSong checks those first).
static boolean IsMp3(const byte *data, int len, mp3dec_frame_info_t *info)
{
    static mp3dec_t probe;      // game thread only
    int start, n;

    if (len < 4)
    {
        return false;
    }

    start = Id3Size(data, len);

    mp3dec_init(&probe);
    n = mp3dec_decode_frame(&probe, data + start, len - start, NULL, info);

    return n > 0 && info->hz > 0 && info->layer == 3;
}

static mp3_song_t *FindMp3(void *handle)
{
    int i;

    for (i = 0; i < MAX_SONGS && handle != NULL; i++)
    {
        if (mp3_songs[i] == handle)
        {
            return mp3_songs[i];
        }
    }

    return NULL;
}

static void *RegisterMp3(const void *data, int len)
{
    mp3_song_t *song;
    int i;

    song = malloc(sizeof(*song));
    if (song == NULL || (song->data = malloc(len)) == NULL)
    {
        PS3_Log("[music] out of memory for a %d KB MP3", len / 1024);
        free(song);
        return NULL;
    }
    memcpy(song->data, data, len);
    song->len = len;
    song->start = Id3Size(song->data, len);

    Lock();
    for (i = 0; i < MAX_SONGS; i++)
    {
        if (mp3_songs[i] == NULL)
        {
            mp3_songs[i] = song;
            break;
        }
    }
    Unlock();

    if (i == MAX_SONGS)
    {
        // no free slot (can't happen: one song at a time)
        free(song->data);
        free(song);
        return NULL;
    }

    return song;
}

static void FreeMp3(mp3_song_t *song)
{
    int i;

    Lock();
    if (mp3_cur == song)
    {
        Mp3Stop();
    }
    for (i = 0; i < MAX_SONGS; i++)
    {
        if (mp3_songs[i] == song)
        {
            mp3_songs[i] = NULL;
        }
    }
    Unlock();

    free(song->data);
    free(song);
}

// ---------------------------------------------------------------------
// The SoundFont player (audio thread)
// ---------------------------------------------------------------------

static void ResetChannels(void)
{
    int ch;

    // tsf_reset frees the channels: 15 first allocates all 16 at once
    tsf_reset(synth);

    for (ch = 15; ch >= 0; ch--)
    {
        tsf_channel_set_presetnumber(synth, ch, 0, ch == 9);
    }
}

static void Dispatch(const tml_message *m)
{
    switch (m->type)
    {
        case TML_PROGRAM_CHANGE:
            tsf_channel_set_presetnumber(synth, m->channel, m->program,
                                         m->channel == 9);
            break;

        case TML_NOTE_ON:
            if (m->velocity == 0)
            {
                tsf_channel_note_off(synth, m->channel, m->key);
            }
            else
            {
                tsf_channel_note_on(synth, m->channel, m->key,
                                    m->velocity / 127.0f);
            }
            break;

        case TML_NOTE_OFF:
            tsf_channel_note_off(synth, m->channel, m->key);
            break;

        case TML_PITCH_BEND:
            tsf_channel_set_pitchwheel(synth, m->channel, m->pitch_bend);
            break;

        case TML_CONTROL_CHANGE:
            tsf_channel_midi_control(synth, m->channel, m->control,
                                     m->control_value);
            break;

        default:
            break;
    }
}

// opl_ps3.c's audio thread, after the OPL music and before the sound
// effects: adds the SoundFont music to the block.
void PS3Music_MixInto(float *dst, unsigned int frames, int ch)
{
    unsigned int done = 0;
    boolean looped = false;

    if (!sf2_mutex_ready || ch != 2)
    {
        return;
    }

    sysMutexLock(sf2_mutex, 0);

    if (mp3_cur != NULL)
    {
        // an MP3 song plays instead of either synth
        if (!song_paused)
        {
            Mp3MixInto(dst, frames);
        }
        sysMutexUnlock(sf2_mutex);
        return;
    }

    if (synth == NULL || song_paused)
    {
        sysMutexUnlock(sf2_mutex);
        return;
    }

    while (done < frames)
    {
        unsigned int n = frames - done;

        if (song_playing)
        {
            while (song_pos != NULL && song_pos->time <= song_ms)
            {
                Dispatch(song_pos);
                song_pos = song_pos->next;
            }

            if (song_pos == NULL)
            {
                // once per block at most: a song with no length would
                // otherwise loop here for ever
                if (song_looping && song_first != NULL && !looped)
                {
                    tsf_note_off_all(synth);
                    song_pos = song_first;
                    song_ms = 0;
                    looped = true;
                    continue;
                }

                if (!song_looping)
                {
                    song_playing = false;
                }
            }
            else
            {
                // up to the next event, so it lands on time
                double left = (song_pos->time - song_ms) * SF2_RATE / 1000.0;
                unsigned int until = (unsigned int) left + 1;

                if (until < n)
                {
                    n = until;
                }
            }
        }

        // after the end too: the last notes ring out
        tsf_render_float(synth, dst + done * 2, (int) n, 1);
        done += n;
        song_ms += n * 1000.0 / SF2_RATE;
    }

    sysMutexUnlock(sf2_mutex);
}

// ---------------------------------------------------------------------
// Songs
// ---------------------------------------------------------------------

static tml_message *LoadMidi(void *data, int len)
{
    tml_message *midi = NULL;

    if (len >= 4 && memcmp(data, "MThd", 4) == 0)
    {
        midi = tml_load_memory(data, len);
    }
    else
    {
        // Doom's MUS: through Crispy's own converter, as i_oplmusic.c
        MEMFILE *in = mem_fopen_read(data, len);
        MEMFILE *out = mem_fopen_write();
        void *buf;
        size_t size;

        if (mus2mid(in, out) == 0)
        {
            mem_get_buf(out, &buf, &size);
            midi = tml_load_memory(buf, (int) size);
        }

        mem_fclose(in);
        mem_fclose(out);
    }

    return midi;
}

// Starts the current song on the SoundFont (under the lock).
static void StartSoundFontSong(void)
{
    int i;

    song_first = NULL;

    for (i = 0; i < MAX_SONGS; i++)
    {
        if (songs[i].handle == current_handle && songs[i].midi != NULL)
        {
            song_first = songs[i].midi;
        }
    }

    ResetChannels();
    song_pos = song_first;
    song_ms = 0;
    song_looping = current_looping;
    song_paused = false;
    song_playing = (song_first != NULL);
}

static void StopSoundFontSong(void)
{
    song_playing = false;
    song_first = song_pos = NULL;

    if (synth != NULL)
    {
        tsf_note_off_all(synth);
    }
}

// ---------------------------------------------------------------------
// The module the engine sees
// ---------------------------------------------------------------------

static boolean PS3Music_Init(void)
{
    sys_mutex_attr_t attr;
    boolean ok;

    // The OPL module opens the audio port and its thread: first.
    ok = music_opl_module.Init();

    sysMutexAttrInitialize(attr);
    sf2_mutex_ready = (sysMutexCreate(&sf2_mutex, &attr) == 0);

    ScanSoundFonts();
    PS3_Log("[music] %d SoundFont(s) in %s, chosen: %s", sf2_count, SF2_DIR,
            ps3_music_sf2[0] ? ps3_music_sf2 : "OPL");

    if (ps3_music_sf2[0] != '\0' && sf2_mutex_ready)
    {
        tsf *f = LoadSoundFont(ps3_music_sf2);

        if (f != NULL)
        {
            synth = f;
            M_StringCopy(synth_name, ps3_music_sf2, sizeof(synth_name));
        }
        else
        {
            // gone or broken: back to OPL, and say so in the menu
            ps3_music_sf2 = M_StringDuplicate("");
        }
    }

    return ok;
}

static void PS3Music_Shutdown(void)
{
    int i;

    // Stops the audio thread (opl_ps3.c): nothing renders after this.
    music_opl_module.Shutdown();

    for (i = 0; i < MAX_SONGS; i++)
    {
        if (songs[i].midi != NULL)
        {
            tml_free(songs[i].midi);
        }
        songs[i].handle = NULL;
        songs[i].midi = NULL;
    }

    song_first = song_pos = NULL;

    mp3_cur = NULL;
    for (i = 0; i < MAX_SONGS; i++)
    {
        if (mp3_songs[i] != NULL)
        {
            free(mp3_songs[i]->data);
            free(mp3_songs[i]);
            mp3_songs[i] = NULL;
        }
    }

    if (synth != NULL)
    {
        tsf_close(synth);
        synth = NULL;
    }
}

static void PS3Music_SetMusicVolume(int volume)
{
    music_opl_module.SetMusicVolume(volume);

    Lock();
    music_volume = volume;
    if (synth != NULL)
    {
        tsf_set_volume(synth, volume / 127.0f);
    }
    Unlock();
}

static void PS3Music_PauseSong(void)
{
    music_opl_module.PauseMusic();

    Lock();
    song_paused = true;
    Unlock();
}

static void PS3Music_ResumeSong(void)
{
    music_opl_module.ResumeMusic();

    Lock();
    song_paused = false;
    Unlock();
}

static void *PS3Music_RegisterSong(void *data, int len)
{
    const byte *b = data;
    void *handle;
    tml_message *midi;
    int i;

    if (len >= 4 && memcmp(data, "MThd", 4) != 0 && memcmp(data, "MUS\x1a", 4) != 0)
    {
        mp3dec_frame_info_t info;

        if (IsMp3(data, len, &info))
        {
            PS3_Log("[music] MP3 song, %d KB, %d Hz, %s, %d kbps",
                    len / 1024, info.hz, info.channels > 1 ? "stereo" : "mono",
                    info.bitrate_kbps);
            return RegisterMp3(data, len);
        }

        PS3_Log("[music] song of %d KB in an unknown format "
                "(%02x %02x %02x %02x %02x %02x %02x %02x): no music",
                len / 1024, b[0], b[1], b[2], b[3],
                len > 4 ? b[4] : 0, len > 5 ? b[5] : 0,
                len > 6 ? b[6] : 0, len > 7 ? b[7] : 0);
        return NULL;
    }

    handle = music_opl_module.RegisterSong(data, len);
    if (handle == NULL)
    {
        return NULL;
    }

    midi = LoadMidi(data, len);
    if (midi == NULL)
    {
        PS3_Log("[music] could not read the song for the SoundFont");
    }

    Lock();
    for (i = 0; i < MAX_SONGS; i++)
    {
        if (songs[i].handle == NULL)
        {
            songs[i].handle = handle;
            songs[i].midi = midi;
            midi = NULL;
            break;
        }
    }
    Unlock();

    if (midi != NULL)
    {
        // no free slot (can't happen: one song at a time)
        tml_free(midi);
    }

    return handle;
}

static void PS3Music_UnRegisterSong(void *handle)
{
    tml_message *midi = NULL;
    mp3_song_t *mp3;
    int i;

    Lock();
    mp3 = FindMp3(handle);
    if (mp3 != NULL && handle == current_handle)
    {
        current_on = false;
        current_handle = NULL;
    }
    Unlock();

    if (mp3 != NULL)
    {
        // ours alone: the OPL module never saw it
        Mp3LogPlayed();
        FreeMp3(mp3);
        return;
    }

    Lock();
    for (i = 0; i < MAX_SONGS; i++)
    {
        if (songs[i].handle == handle && handle != NULL)
        {
            midi = songs[i].midi;
            if (midi != NULL && midi == song_first)
            {
                StopSoundFontSong();
            }
            songs[i].handle = NULL;
            songs[i].midi = NULL;
        }
    }
    if (handle == current_handle)
    {
        current_on = false;
        current_handle = NULL;
    }
    Unlock();

    if (midi != NULL)
    {
        tml_free(midi);
    }

    music_opl_module.UnRegisterSong(handle);
}

static void PS3Music_PlaySong(void *handle, boolean looping)
{
    boolean soundfont, mp3_ok = false;
    mp3_song_t *mp3;

    Lock();
    current_handle = handle;
    current_looping = looping;
    current_on = true;
    mp3 = FindMp3(handle);
    Mp3Stop();
    if (mp3 != NULL)
    {
        // whatever the Music setting: neither synth plays this one
        StopSoundFontSong();
        Mp3Start(mp3, looping);
        mp3_ok = (mp3_cur != NULL);
        soundfont = false;
    }
    else
    {
        soundfont = (synth != NULL);
        if (soundfont)
        {
            StartSoundFontSong();
        }
    }
    Unlock();

    if (mp3 != NULL)
    {
        music_opl_module.StopSong();
        if (mp3_ok)
        {
            PS3_Log("[music] MP3 playing%s, music volume %d/127",
                    looping ? " (loop)" : "", music_volume);
        }
        else
        {
            PS3_Log("[music] the MP3 song has no frame to play");
        }
    }
    else if (!soundfont)
    {
        music_opl_module.PlaySong(handle, looping);
    }
}

static void PS3Music_StopSong(void)
{
    music_opl_module.StopSong();

    Lock();
    current_on = false;
    StopSoundFontSong();
    Mp3Stop();
    Unlock();
}

static boolean PS3Music_MusicIsPlaying(void)
{
    boolean playing;

    Lock();
    playing = (synth != NULL) ? song_playing : false;
    if (mp3_cur != NULL)
    {
        playing = true;
    }
    Unlock();

    return playing || music_opl_module.MusicIsPlaying();
}

// the OPL module's: this one only adds to it
static const snddevice_t music_ps3_devices[] =
{
    SNDDEVICE_ADLIB,
    SNDDEVICE_SB,
};

const music_module_t music_ps3_module =
{
    music_ps3_devices,
    arrlen(music_ps3_devices),
    PS3Music_Init,
    PS3Music_Shutdown,
    PS3Music_SetMusicVolume,
    PS3Music_PauseSong,
    PS3Music_ResumeSong,
    PS3Music_RegisterSong,
    PS3Music_UnRegisterSong,
    PS3Music_PlaySong,
    PS3Music_StopSong,
    PS3Music_MusicIsPlaying,
    NULL,  // Poll
};

// ---------------------------------------------------------------------
// The menu (Crispness -> Audible -> Music)
// ---------------------------------------------------------------------

// What the menu shows: "OPL", or the SoundFont's name without ".sf2",
// cut to fit the line.
const char *PS3_Music_SynthName(void)
{
    static char label[24];
    size_t n;

    if (ps3_music_sf2 == NULL || ps3_music_sf2[0] == '\0')
    {
        return "OPL";
    }

    M_StringCopy(label, ps3_music_sf2, sizeof(label));
    n = strlen(ps3_music_sf2);
    if (n > 4 && n - 4 < sizeof(label))
    {
        label[n - 4] = '\0';
    }

    return label;
}

// dir > 0: next in OPL, first .sf2, second .sf2 ... dir < 0: previous.
// Loads the new SoundFont (a few seconds for a big one), swaps it in
// and starts the current song over on it.
void PS3_Music_CycleSynth(int dir)
{
    int cur = 0, next, i;
    tsf *f = NULL, *old;

    if (!sf2_mutex_ready)
    {
        return;
    }

    ScanSoundFonts();

    for (i = 0; i < sf2_count; i++)
    {
        if (ps3_music_sf2 != NULL && strcasecmp(sf2_names[i], ps3_music_sf2) == 0)
        {
            cur = i + 1;
        }
    }

    next = (cur + (dir < 0 ? -1 : 1) + sf2_count + 1) % (sf2_count + 1);

    if (next == cur)
    {
        return;         // no SoundFont to switch to
    }

    if (next > 0)
    {
        f = LoadSoundFont(sf2_names[next - 1]);
        if (f == NULL)
        {
            next = 0;   // broken file: OPL
        }
    }

    // Stop whichever synth is playing, then swap.
    music_opl_module.StopSong();

    Lock();
    StopSoundFontSong();
    old = synth;
    synth = f;
    M_StringCopy(synth_name, next > 0 ? sf2_names[next - 1] : "",
                 sizeof(synth_name));
    Unlock();

    if (old != NULL)
    {
        // the memory back: a big SoundFont is a good part of the PS3's
        tsf_close(old);
    }

    ps3_music_sf2 = M_StringDuplicate(next > 0 ? sf2_names[next - 1] : "");
    PS3_Log("[music] now %s", next > 0 ? ps3_music_sf2 : "OPL");

    // an MP3 song is not the synth's: it just goes on
    if (current_on && current_handle != NULL && FindMp3(current_handle) == NULL)
    {
        PS3Music_PlaySong(current_handle, current_looping);
    }
}
