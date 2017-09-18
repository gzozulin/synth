#include <assert.h>
#include <stdbool.h>
#include <float.h>

#include "c11threads.h"

#include "SDL2/SDL.h"

// -------------------------- +Const --------------------------

SDL_Window    *g_window             = NULL;
SDL_Renderer  *g_renderer           = NULL;

#define       FREQUENCY             44100
#define       SAMPLES               512

#define       TICK_TIME             (1.0f / 60.0f)
#define       SAMPLE_TIME           (1.0f / (float) FREQUENCY)

#define       AUDIO_DEV_ID          1

#define       PI                    ((float) M_PI)

char          g_logBuffer[1024];

bool          g_quit                = false;

float         g_audioBuffer[2048];

#define       KEYS_NUM              16
const char    *g_keys               = "zsxcfvgbnjmk,l./";

#define       NOTES_NUM             KEYS_NUM

bool          g_leftShift           = false;

// -------------------------- +Common --------------------------

extern inline float synth_appGetTime()
{
    return (float) SDL_GetTicks() / 1000.0f;
}

extern inline void  synth_appSleep(float seconds)
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
        {
            printf("%.2f -> INFO -> %s:%d %s\n", synth_appGetTime(), file, line, g_logBuffer);
            break;
        }
        case LOG_LEVEL_ERROR:
        {
            printf("%.2f -> ERROR -> %s:%d %s\n", synth_appGetTime(), file, line, g_logBuffer);
            exit(-1);
        }
    }
}

#define logi(...) logline(LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__);
#define loge(...) logline(LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__);

#define SDL_FAIL() { loge("SDL error: %s", SDL_GetError()); }
#define SDL_ENFORCE(expr) { if ((expr) < 0)  SDL_FAIL(); }
#define SDL_ENFORCE_PTR(ptr) { if ((ptr) == NULL) SDL_FAIL(); }

// -------------------------- +Synth --------------------------

extern inline float synth_convertFrequency(float hertz) { return hertz * 2.0f * PI; }
extern inline float synth_scaleNote(int note) { return 256 * powf(1.0594630943592952645618252949463f, note); }

struct synth_Note
{
    int id;
    float on;
    float off;
    int channel;
};

mtx_t g_notesMutex;
struct synth_Note *g_notes[] =
{
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL
};

void synth_createNotesMutex()
{
    mtx_init(&g_notesMutex, mtx_plain);
}

void synth_destroyNotesMutex()
{
    mtx_destroy(&g_notesMutex);
}

enum synth_WaveType
{
    WAVE_TYPE_SINE,
    WAVE_TYPE_SQUARE,
    WAVE_TYPE_TRIANGLE,
    WAVE_TYPE_SAW_ANALOGUE,
    WAVE_TYPE_SAW_DIGITAL,
    WAVE_TYPE_NOISE
};

float synth_oscillate(const float time, const float freq, const enum synth_WaveType type, const float lfoFreq, const float lfoAmplitude, float custom)
{
    const float dFreq = synth_convertFrequency(freq) * time + lfoAmplitude * freq * (sinf(synth_convertFrequency(lfoFreq) * time));
    switch (type) {
        case WAVE_TYPE_SINE: // Sine wave bewteen -1 and +1
        {
            return sinf(dFreq);
        }
        case WAVE_TYPE_SQUARE: // Square wave between -1 and +1
        {
            return sinf(dFreq) > 0 ? 1.0f : -1.0f;
        }
        case WAVE_TYPE_TRIANGLE: // Triangle wave between -1 and +1
        {
            return asinf(sinf(dFreq)) * (2.0f / PI);
        }
        case WAVE_TYPE_SAW_ANALOGUE: // Saw wave (analogue / warm / slow)
        {
            float dOutput = 0.0;
            float  n = 1.0f;
            while (n < custom) {
                dOutput += (sinf(n * dFreq)) / n;
                n += 1.0f;
            }
            return dOutput * (2.0f / PI);
        }
        case WAVE_TYPE_SAW_DIGITAL:
        {
            return (2.0f / PI) * (freq * PI * fmodf(time, 1.0f / freq) - (PI / 2.0f));
        }
        case WAVE_TYPE_NOISE:
        {
            return 2.0f * ((float) random() / (float) RAND_MAX) - 1.0f;
        }
        default:
        {
            loge("Unknown type!");
            return 0.0f;
        }
    }
}

struct synth_Envelope
{
    float attackTime;
    float decayTime;
    float releaseTime;
    float startAmplitude;
    float sustainAmplitude;
};

float synth_envelopeGetAmplitude(struct synth_Envelope *envelope, const float time, const float timeOn, const float timeOff)
{
    assert(envelope != NULL);
    float amplitude;
    const bool noteIsOn = timeOn > timeOff;
    if (noteIsOn) {
        float lifetime = time - timeOn;
        if (lifetime <= envelope->attackTime) {
            amplitude = (lifetime / envelope->attackTime) * envelope->startAmplitude;
        }else if (lifetime <= (envelope->attackTime + envelope->decayTime)) {
            amplitude = ((lifetime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;
        } else {
            amplitude = envelope->sustainAmplitude;
        }
    } else {
        float releaseAmplitude;
        float lifetime = timeOff - timeOn;
        if (lifetime <= envelope->attackTime) {
            releaseAmplitude = (lifetime / envelope->attackTime) * envelope->startAmplitude;
        } else if (lifetime <= (envelope->attackTime + envelope->decayTime)) {
            releaseAmplitude = ((lifetime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;
        } else {
            releaseAmplitude = envelope->sustainAmplitude;
        }
        amplitude = ((time - timeOff) / envelope->releaseTime) * (0.0f - releaseAmplitude) + releaseAmplitude;
    }
    if (amplitude <= FLT_EPSILON) {
        amplitude = 0.0f;
    }
    return amplitude;
}

float synth_voiceBell(struct synth_Envelope *envelope, float volume, float time, struct synth_Note *note, bool *isFinished)
{
    assert(envelope != NULL);
    const float amplitude = synth_envelopeGetAmplitude(envelope, time, note->on, note->off);
    if (amplitude <= 0.0) {
        *isFinished = true;
        return 0.0f;
    }
    const float sound =
            + 1.00f * synth_oscillate(time, synth_scaleNote(note->id + 12), WAVE_TYPE_SINE, 5.0f, 0.001f, 50.0f)
            + 0.50f * synth_oscillate(time, synth_scaleNote(note->id + 24), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f)
            + 0.25f * synth_oscillate(time, synth_scaleNote(note->id + 36), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f);
    return amplitude * sound * volume;
}

struct synth_Envelope g_envelopeBell = { 0.01f, 1.0f, 1.0f, 1.0f, 0.0f };

float synth_voiceHarmonica(struct synth_Envelope *envelope, float volume, float time, struct synth_Note *note, bool *isFinished)
{
    assert(envelope != NULL);
    const float amplitude = synth_envelopeGetAmplitude(envelope, time, note->on, note->off);
    if (amplitude <= 0.0) {
        *isFinished = true;
        return 0.0f;
    }
    const float sound =
            + 1.00f * synth_oscillate(time, synth_scaleNote(note->id), WAVE_TYPE_SQUARE, 5.0, 0.001, 50.0f)
            + 0.50f * synth_oscillate(time, synth_scaleNote(note->id + 12), WAVE_TYPE_SQUARE, 0.0f, 0.0f, 50.0f)
            + 0.05f  * synth_oscillate(time, synth_scaleNote(note->id + 24), WAVE_TYPE_NOISE, 0.0f, 0.0f, 50.0f);
    return amplitude * sound * volume;
}

struct synth_Envelope g_envelopeHarmonica = { 0.05f, 1.0f, 0.1f, 1.0f, 0.95f };

// -------------------------- +Audio --------------------------

float synth_audioSampleCreate(float time)
{
    mtx_lock(&g_notesMutex);
    float mixedOutput = 0.0;
    for (int i = 0; i < NOTES_NUM; i++) {
        struct synth_Note *note = g_notes[i];
        if (note == NULL) {
            continue;
        }
        bool noteFinished = false;
        float dSound = 0;
        switch (note->channel) {
            case 0: dSound = synth_voiceHarmonica(&g_envelopeHarmonica, 0.5f, time, note, &noteFinished); break;
            case 1: dSound = synth_voiceBell(&g_envelopeBell, 0.5f, time, note, &noteFinished); break;
            default: loge("Unknown channel!"); break;
        }
        mixedOutput += dSound;
        if (noteFinished && note->off > note->on) {
            g_notes[i] = NULL;
            free(note);
        }
    }
    mtx_unlock(&g_notesMutex);
    return mixedOutput;
}

void synth_audioAppendBuffer(SDL_AudioDeviceID dev, float start, float *accumulator)
{
    uint index = 0;
    while (*accumulator > SAMPLE_TIME) {
        const float time = start + index * SAMPLE_TIME;
        g_audioBuffer[index] = synth_audioSampleCreate(time);
        *accumulator -= SAMPLE_TIME;
        index++;
    }
    SDL_ENFORCE(SDL_QueueAudio(dev, g_audioBuffer, index * sizeof(float)));
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

void synth_appHandleKey(SDL_Keycode keysym, bool pressed, float time)
{
    for (int k = 0; k < KEYS_NUM; k++)
    {
        char key = g_keys[k];
        if (keysym != key) {
            continue;
        }
        mtx_lock(&g_notesMutex);
        struct synth_Note *note = NULL;
        for (int i = 0; i < NOTES_NUM; ++i) {
            if (g_notes[i] != NULL && g_notes[i]->id == k) {
                note = g_notes[i];
                break;
            }
        }
        if (note == NULL) {
            if (pressed) {
                for (int i = 0; i < NOTES_NUM; i++) {
                    if (g_notes[i] == NULL) {
                        g_notes[i] = malloc(sizeof(struct synth_Note));
                        g_notes[i]->id = k;
                        g_notes[i]->on = time;
                        g_notes[i]->channel = g_leftShift ? 0 : 1;
                        break;
                    }
                }
            }
        } else {
            if (pressed) {
                if (note->off > note->on) {
                    note->on = time;
                }
            } else {
                if (note->off < note->on) {
                    note->off = time;
                }
            }
        }
        mtx_unlock(&g_notesMutex);
    }
}

void synth_appPollEvents(float time)
{
    SDL_Event event;
    while( SDL_PollEvent(&event) != 0 ) {
        if(event.type == SDL_QUIT || event.key.keysym.sym == SDLK_ESCAPE) {
            g_quit = true;
        } else if (event.key.keysym.sym == SDLK_LSHIFT) {
            g_leftShift = event.type == SDL_KEYDOWN;
        } else if (event.type == SDL_KEYDOWN) {
            synth_appHandleKey(event.key.keysym.sym, true, time);
        } else if (event.type == SDL_KEYUP) {
            synth_appHandleKey(event.key.keysym.sym, false, time);
        }
    }
}

void synth_appSleepIfNeeded(float start)
{
    const float finish = synth_appGetTime();
    const float sleep = TICK_TIME - (finish - start);
    if (sleep > 0) {
        synth_appSleep(sleep);
    }
}

void synth_appRunLoop()
{
    logi("synth_appRunLoop() called");
    float accumulator = 0.0f;
    float last = synth_appGetTime();
    while (!g_quit) {
        const float start = synth_appGetTime();
        synth_appPollEvents(start);
        const float elapsed = start - last;
        accumulator += elapsed;
        synth_audioAppendBuffer(AUDIO_DEV_ID, last, &accumulator);
        last = start;
        synth_appSleepIfNeeded(start);
    }
}

void synth_appPringKeysLayout()
{
    logi("|   |   |   |   |   | |   |   |   |   | |   | |   |   |   |");
    logi("|   | S |   |   | F | | G |   |   | J | | K | | L |   |   |");
    logi("|   |___|   |   |___| |___|   |   |___| |___| |___|   |   |__");
    logi("|     |     |     |     |     |     |     |     |     |     |");
    logi("|  Z  |  X  |  C  |  V  |  B  |  N  |  M  |  ,  |  .  |  /  |");
    logi("|_____|_____|_____|_____|_____|_____|_____|_____|_____|_____|");
}

// -------------------------- +Main --------------------------

int main()
{
    synth_createNotesMutex();
    synth_appWinCreate();
    synth_audioDevicePrepare();
    synth_appPringKeysLayout();
    SDL_PauseAudio(0);
    synth_appRunLoop();
    SDL_CloseAudio();
    SDL_Quit();
    synth_destroyNotesMutex();
    return 0;
}
