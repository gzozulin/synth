#include <assert.h>
#include <stdbool.h>
#include <float.h>
#include "c11threads.h"

#include "SDL2/SDL.h"

// -------------------------- +Const --------------------------

typedef float FTYPE;

#define       FREQUENCY             44100
#define       SAMPLES               512

#define       TICK_TIME             (1.0f / 60.0f)

#define       SAMPLE_TIME           (1.0f / (FTYPE) FREQUENCY)

#define       AUDIO_DEV_ID          1

#define       PI                    ((float) M_PI)

char          g_logBuffer[1024];

SDL_Window    *g_window             = NULL;
SDL_Renderer  *g_renderer           = NULL;

bool          g_quit                = false;

FTYPE         g_audioBuffer[2048];

#define       KEYS_NUM              16
const char    *g_keys               = "zsxcfvgbnjmk\xbcl\xbe\xbf";

#define       NOTES_NUM             16

// -------------------------- +Common --------------------------

extern inline FTYPE synth_appGetTime()
{
    return (FTYPE) SDL_GetTicks() / 1000.0f;
}

extern inline void  synth_appSleep(FTYPE seconds)
{
    const long millis = (const long) (seconds * 1e+6);
    const long nanos = millis * 1000L;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = nanos };
    thrd_sleep(&ts, NULL);
}

enum synth_LogLevel
{
    LOG_LEVEL_INFO,
    LOG_LEVEL_ERROR
};

void logline(enum synth_LogLevel level, const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsprintf(g_logBuffer, fmt, args);
    va_end(args);
    switch (level) {
        case LOG_LEVEL_INFO:
            printf("%.2f -> INFO -> %s:%d %s\n", synth_appGetTime(), file, line, g_logBuffer);
            break;
        case LOG_LEVEL_ERROR:
            printf("%.2f -> ERROR -> %s:%d %s\n", synth_appGetTime(), file, line, g_logBuffer);
            exit(-1);
    }
}

#define logi(...) logline(LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__);
#define loge(...) logline(LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__);

#define SDL_FAIL() { loge("SDL error: %s", SDL_GetError()); }
#define SDL_ENFORCE(expr) { if ((expr) < 0)  SDL_FAIL(); }
#define SDL_ENFORCE_PTR(ptr) { if ((ptr) == NULL) SDL_FAIL(); }

// -------------------------- +Synth --------------------------

extern inline FTYPE synth_convertFrequency(float hertz) { return hertz * 2.0f * PI; }

extern inline float synth_scaleNote(int note)
{
    const float baseFrequency = 220.0f; // A2
    const float twelwthRootOf2 = powf(2.0f, 1.0f / 12.0f);
    return baseFrequency * powf(twelwthRootOf2, note);
}

struct synth_Note
{
    int id;
    FTYPE on;
    FTYPE off;
    bool active;
    int channel;
};

struct synth_Note *g_notes[] =
{
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL
};

enum synth_WaveType
{
    WAVE_TYPE_SINE,
    WAVE_TYPE_SQUARE,
    WAVE_TYPE_TRIANGLE,
    WAVE_TYPE_SAW_ANALOGUE,
    WAVE_TYPE_SAW_DIGITAL,
    WAVE_TYPE_NOISE
};

float synth_oscillate(const FTYPE time, const FTYPE freq, const enum synth_WaveType type, const FTYPE lfoFreq, const FTYPE lfoAmplitude, FTYPE custom)
{
    FTYPE dFreq = synth_convertFrequency(freq) * time + lfoAmplitude * freq * (sinf(synth_convertFrequency(lfoFreq) * time));// osc(time, lfoFreq, OSC_SINE);

    switch (type)
    {
        case WAVE_TYPE_SINE: // Sine wave bewteen -1 and +1
            return sinf(dFreq);

        case WAVE_TYPE_SQUARE: // Square wave between -1 and +1
            return sinf(dFreq) > 0 ? 1.0f : -1.0f;

        case WAVE_TYPE_TRIANGLE: // Triangle wave between -1 and +1
            return asinf(sinf(dFreq)) * (2.0f / PI);

        case WAVE_TYPE_SAW_ANALOGUE: // Saw wave (analogue / warm / slow)
        {
            FTYPE dOutput = 0.0;
            FTYPE  n = 1.0f;
            while (n < custom) {
                dOutput += (sinf(n * dFreq)) / n;
                n += 1.0f;
            }
            return dOutput * (2.0f / PI);
        }

        case WAVE_TYPE_SAW_DIGITAL:
            return (2.0f / PI) * (freq * PI * fmodf(time, 1.0f / freq) - (PI / 2.0f));

        case WAVE_TYPE_NOISE:
            return 2.0f * ((FTYPE) random() / (FTYPE) RAND_MAX) - 1.0f;

        default:
            loge("Unknown type!");
            return 0.0f;
    }
}

struct synth_Envelope
{
    FTYPE attackTime;
    FTYPE decayTime;
    FTYPE releaseTime;
    FTYPE startAmplitude;
    FTYPE sustainAmplitude;
};

float synth_envelopeGetAmplitude(struct synth_Envelope *envelope, const FTYPE time, const FTYPE timeOn, const FTYPE timeOff)
{
    assert(envelope != NULL);
    
    FTYPE amplitude = 0.0f;

    if (timeOn > timeOff) // Note is on
    {
        FTYPE lifetime = time - timeOn;

        if (lifetime <= envelope->attackTime) {
            amplitude = (lifetime / envelope->attackTime) * envelope->startAmplitude;
        }

        if (lifetime > envelope->attackTime && lifetime <= (envelope->attackTime + envelope->decayTime)) {
            amplitude = ((lifetime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;
        }

        if (lifetime > (envelope->attackTime + envelope->decayTime)) {
            amplitude = envelope->sustainAmplitude;
        }

    }
    else // Note is off
    {
        FTYPE releaseAmplitude = 0.0f;
        FTYPE lifetime = timeOff - timeOn;

        if (lifetime <= envelope->attackTime) {
            releaseAmplitude = (lifetime / envelope->attackTime) * envelope->startAmplitude;
        }

        if (lifetime > envelope->attackTime && lifetime <= (envelope->attackTime + envelope->decayTime)) {
            releaseAmplitude = ((lifetime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;
        }

        if (lifetime > (envelope->attackTime + envelope->decayTime)) {
            releaseAmplitude = envelope->sustainAmplitude;
        }

        amplitude = ((time - timeOff) / envelope->releaseTime) * (0.0f - releaseAmplitude) + releaseAmplitude;
    }

    // Amplitude should not be negative
    if (amplitude <= FLT_EPSILON) {
        amplitude = 0.0f;
    }

    return amplitude;
}

FTYPE synth_voiceBell(struct synth_Envelope *envelope, float volume, float time, struct synth_Note *note, bool *isFinished)
{
    assert(envelope != NULL);
    FTYPE amplitude = synth_envelopeGetAmplitude(envelope, time, note->on, note->off);
    if (amplitude <= 0.0) {
        *isFinished = true;
    }

    const FTYPE sound =
            + 1.0f * synth_oscillate(time, synth_scaleNote(note->id + 12), WAVE_TYPE_SINE, 5.0f, 0.001f, 50.0f)
            + 0.5f * synth_oscillate(time, synth_scaleNote(note->id + 24), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f)
            + 0.25f * synth_oscillate(time, synth_scaleNote(note->id + 36), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f);

    return amplitude * sound * volume;
}

struct synth_Envelope g_envelopeBell = { 0.01f, 1.0f, 1.0f, 1.0f, 0.0f };

FTYPE synth_voiceHarmonica(struct synth_Envelope *envelope, float volume, float time, struct synth_Note *note, bool *isFinished)
{
    assert(envelope != NULL);
    FTYPE amplitude = synth_envelopeGetAmplitude(envelope, time, note->on, note->off);
    if (amplitude <= 0.0) {
        *isFinished = true;
    }

    FTYPE sound =
            + 1.00f * synth_oscillate(time, synth_scaleNote(note->id + 12), WAVE_TYPE_SINE, 5.0f, 0.001f, 50.0f)
            + 0.50f * synth_oscillate(time, synth_scaleNote(note->id + 24), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f)
            + 0.25f * synth_oscillate(time, synth_scaleNote(note->id + 36), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f);

    return amplitude * sound * volume;
}

struct synth_Envelope g_envelopeHarmonica = { 0.01f, 1.0f, 1.0f, 1.0f, 0.0f };

// -------------------------- +Audio --------------------------

FTYPE synth_audioSampleCreate(FTYPE time)
{
    //todo: unique_lock<mutex> lm(muxNotes);
    FTYPE dMixedOutput = 0.0;
    for (int i = 0; i < NOTES_NUM; i++) {
        struct synth_Note *note = g_notes[i];
        if (note == NULL) {
            continue;
        }
        bool noteFinished = false;
        FTYPE dSound = 0;
        if(note->channel == 2) {
            dSound = synth_voiceBell(&g_envelopeBell, 1.0f, time, note, &noteFinished);
        }
        if (note->channel == 1) {
            dSound = synth_voiceHarmonica(&g_envelopeHarmonica, 0.4f, time, note, &noteFinished) * 0.5f;
        }
        dMixedOutput += dSound;
        if (noteFinished && note->off > note->on) {
            note->active = false;
        }
    }
    for (int i = 0; i < NOTES_NUM; i++) {
        struct synth_Note *note = g_notes[i];
        if (note != NULL && !note->active) {
            g_notes[i] = NULL;
            free(note);
        }
    }
    return dMixedOutput;
}

void synth_audioAppendBuffer(SDL_AudioDeviceID dev, FTYPE start, FTYPE *accumulator)
{
    uint index = 0;
    while (*accumulator > SAMPLE_TIME) {
        const FTYPE time = start + index * SAMPLE_TIME;
        g_audioBuffer[index] = synth_audioSampleCreate(time);
        *accumulator -= SAMPLE_TIME;
        index++;
    }
    SDL_ENFORCE(SDL_QueueAudio(dev, g_audioBuffer, index));
}

void synth_audioDeviceList()
{
    const int num = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < num; ++i) {
        logi("Found audio device: %s", SDL_GetAudioDeviceName(i, 0));
    }
}

void synth_audioDevicePrintSpec(SDL_AudioSpec *spec)
{
    assert(spec != NULL);
    logi("Received freq: %d", spec->freq);
    logi("Received format: %d", spec->format);
    logi("Received channels: %d", spec->channels);
    logi("Received samples: %d", spec->samples);
}

void synth_audioDevicePrepare()
{
    synth_audioDeviceList();
    SDL_AudioSpec asked, received;
    memset(&asked, 0, sizeof(asked));
    memset(&received, 0, sizeof(received));
    asked.freq = FREQUENCY;
    asked.format = AUDIO_F32;
    asked.channels = 1;
    asked.samples = SAMPLES;
    asked.callback = NULL;
    SDL_ENFORCE(SDL_OpenAudio(&asked, &received));
    logi("Asked:")
    synth_audioDevicePrintSpec(&asked);
    logi("Received:")
    synth_audioDevicePrintSpec(&received);
}

// -------------------------- +Application --------------------------

void synth_appWinCreate()
{
    SDL_ENFORCE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER));
    g_window = SDL_CreateWindow( "SDL Synth", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 300, 200, SDL_WINDOW_SHOWN);
    SDL_ENFORCE_PTR(g_window);
    g_renderer = SDL_CreateRenderer(g_window, -1, 0);
    SDL_ENFORCE_PTR(g_window);
    SDL_ENFORCE(SDL_SetRenderDrawColor(g_renderer, 255, 255, 0, 255));
    SDL_ENFORCE(SDL_RenderClear(g_renderer));
    SDL_RenderPresent(g_renderer);
}

void synth_appHandleKey(SDL_Keycode keysym, bool pressed, FTYPE time)
{
    for (int k = 0; k < KEYS_NUM; k++)
    {
        char key = g_keys[k];
        if (keysym != key) {
            continue;
        }

        // Check if note already exists in currently playing notes
        //muxNotes.lock();

        struct synth_Note *note = NULL;
        for (int i = 0; i < NOTES_NUM; ++i) {
            if (g_notes[i] != NULL && g_notes[i]->id == k) {
                note = g_notes[i];
                break;
            }
        }

        if (note == NULL)
        {
            // Note not found in vector

            if (pressed)
            {
                // Key has been pressed so create a new note

                for (int i = 0; i < NOTES_NUM; i++) {

                    if (g_notes[i] == NULL) {
                        g_notes[i] = malloc(sizeof(struct synth_Note));

                        //logi("creating")

                        g_notes[i]->id = k;
                        g_notes[i]->on = time;
                        g_notes[i]->channel = 1;
                        g_notes[i]->active = true;

                        break;
                    }
                }
            }
        }
        else
        {
            // Note exists in vector
            if (pressed)
            {
                // Key is still held, so do nothing
                if (note->off > note->on)
                {
                    // Key has been pressed again during release phase
                    note->on = time;
                    note->active = true;
                }
            }
            else
            {
                // Key has been released, so switch off
                if (note->off < note->on)
                {
                    note->off = time;
                }
            }
        }
        //muxNotes.unlock();
    }
}

void synth_appPollEvents(FTYPE time)
{
    SDL_Event event;
    while( SDL_PollEvent(&event) != 0 ) {
        if(event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == 27)) {
            g_quit = true;
        } else if (event.type == SDL_KEYDOWN) {
            synth_appHandleKey(event.key.keysym.sym, true, time);
        } else if (event.type == SDL_KEYUP) {
            synth_appHandleKey(event.key.keysym.sym, false, time);
        }
    }
}

void synth_appSleepIfNeeded(FTYPE start)
{
    const FTYPE finish = synth_appGetTime();
    const FTYPE sleep = TICK_TIME - (finish - start);
    if (sleep > 0) {
        synth_appSleep(sleep);
    }
}

void synth_appRunLoop()
{
    logi("synth_appRunLoop() called");
    FTYPE accumulator = 0.0f;
    FTYPE last = synth_appGetTime();
    while (!g_quit) {
        const FTYPE start = synth_appGetTime();
        synth_appPollEvents(start);
        const FTYPE elapsed = start - last;
        accumulator += elapsed;
        synth_audioAppendBuffer(AUDIO_DEV_ID, last, &accumulator);
        last = start;
        synth_appSleepIfNeeded(start);
    }
}

// -------------------------- +Main --------------------------

int main()
{
    synth_appWinCreate();
    synth_audioDevicePrepare();
    SDL_PauseAudio(0);
    synth_appRunLoop();
    SDL_CloseAudio();
    SDL_Quit();
    return 0;
}
