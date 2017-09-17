#include <assert.h>
#include <stdbool.h>
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

// -------------------------- +Variables --------------------------

char          g_logBuffer[1024];

SDL_Window    *g_window             = NULL;
SDL_Renderer  *g_renderer           = NULL;

bool          g_quit                = false;

FTYPE  g_audioBuffer[2048];

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

extern inline FTYPE synth_scale(const int noteId) { return 256 * powf(1.0594630943592952645618252949463f, noteId); }

struct synth_Note
{
    int id;
    FTYPE on;
    FTYPE off;
    bool active;
    int channel;
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
            return 0.0;
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
    
    FTYPE dAmplitude = 0.0;
    FTYPE dReleaseAmplitude = 0.0;

    if (timeOn > timeOff) // Note is on
    {
        FTYPE dLifeTime = time - timeOn;

        if (dLifeTime <= envelope->attackTime)
            dAmplitude = (dLifeTime / envelope->attackTime) * envelope->startAmplitude;

        if (dLifeTime > envelope->attackTime && dLifeTime <= (envelope->attackTime + envelope->decayTime))
            dAmplitude = ((dLifeTime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;

        if (dLifeTime > (envelope->attackTime + envelope->decayTime))
            dAmplitude = envelope->sustainAmplitude;
    }
    else // Note is off
    {
        FTYPE dLifeTime = timeOff - timeOn;

        if (dLifeTime <= envelope->attackTime)
            dReleaseAmplitude = (dLifeTime / envelope->attackTime) * envelope->startAmplitude;

        if (dLifeTime > envelope->attackTime && dLifeTime <= (envelope->attackTime + envelope->decayTime))
            dReleaseAmplitude = ((dLifeTime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;

        if (dLifeTime > (envelope->attackTime + envelope->decayTime))
            dReleaseAmplitude = envelope->sustainAmplitude;

        dAmplitude = ((time - timeOff) / envelope->releaseTime) * (0.0f - dReleaseAmplitude) + dReleaseAmplitude;
    }

    // Amplitude should not be negative
    if (dAmplitude <= FLT_EPSILON)
        dAmplitude = 0.0;

    return dAmplitude;
}

struct synth_Instrument
{
    FTYPE volume;
    struct synth_Envelope envelope;
};

FTYPE synth_voiceBell(struct synth_Instrument * instrument, float time, struct synth_Note *note, bool *isFinished)
{
    assert(instrument != NULL);
    FTYPE dAmplitude = synth_envelopeGetAmplitude(&instrument->envelope, time, note->on, note->off);
    if (dAmplitude <= 0.0) {
        *isFinished = true;
    }

    FTYPE dSound =
            + 1.0f * synth_oscillate(note->on - time, synth_scale(note->id + 12), WAVE_TYPE_SINE, 5.0f, 0.001f, 50.0f)
            + 0.5f * synth_oscillate(note->on - time, synth_scale(note->id + 24), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f)
            + 0.25f * synth_oscillate(note->on - time, synth_scale(note->id + 36), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f);

    return dAmplitude * dSound * instrument->volume;
}

struct synth_Instrument g_instrumentBell = { 1.0f, { 0.01f, 1.0f, 1.0f, 1.0f, 0.0f } };

FTYPE synth_voiceHarmonica(struct synth_Instrument * instrument, float time, struct synth_Note *note, bool *isFinished)
{
    assert(instrument != NULL);
    FTYPE dAmplitude = synth_envelopeGetAmplitude(&instrument->envelope, time, note->on, note->off);
    if (dAmplitude <= 0.0) {
        *isFinished = true;
    }

    FTYPE dSound =
            + 1.0f * synth_oscillate(note->on - time, synth_scale(note->id + 12), WAVE_TYPE_SINE, 5.0f, 0.001f, 50.0f)
            + 0.5f * synth_oscillate(note->on - time, synth_scale(note->id + 24), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f)
            + 0.25f * synth_oscillate(note->on - time, synth_scale(note->id + 36), WAVE_TYPE_SINE, 0.0f, 0.0f, 50.0f);

    return dAmplitude * dSound * instrument->volume;
}

struct synth_Instrument g_instrumentHarmonica = { 1.0f, { 0.01f, 1.0f, 1.0f, 1.0f, 0.0f } };

// -------------------------- +Audio --------------------------

#define NOTES_NUM 16
struct synth_Note *g_notes[] =
{
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL
};

FTYPE synth_audioSampleCreate(FTYPE time)
{
    //todo: unique_lock<mutex> lm(muxNotes);
    FTYPE dMixedOutput = 0.0;

    for (int i = 0; i < NOTES_NUM; i++) {

        struct synth_Note *note = g_notes[i];
        if (note == NULL) {
            continue;
        }

        bool bNoteFinished = false;
        FTYPE dSound = 0;
        if(note->channel == 2)
            dSound = synth_voiceBell(&g_instrumentBell, time, note, &bNoteFinished);
        if (note->channel == 1)
            dSound = synth_voiceHarmonica(&g_instrumentHarmonica, time, note, &bNoteFinished) * 0.5f;
        dMixedOutput += dSound;

        if (bNoteFinished && note->off > note->on)
            note->active = false;
    }

    for (int i = 0; i < NOTES_NUM; i++) {
        struct synth_Note *note = g_notes[i];
        if (note != NULL && !note->active) {
            free(note);
            g_notes[i] = NULL;
        }
    }

    return dMixedOutput * 0.2f;
}

void synth_audioAppendBuffer(SDL_AudioDeviceID dev, FTYPE start, FTYPE *accumulator)
{
    uint index = 0;
    while (*accumulator > SAMPLE_TIME) {
        const FTYPE time = start + index * SAMPLE_TIME;
        const FTYPE sample = synth_audioSampleCreate(time);
        g_audioBuffer[index] = sample;
        *accumulator -= SAMPLE_TIME;
        index++;
    }
    SDL_ENFORCE(SDL_QueueAudio(dev, g_audioBuffer, index + 1));
}

void synth_audioDeviceList()
{
    logi("synth_audioDeviceList()");
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
    logi("synth_audioDevicePrepare()");
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
    logi("synth_appWinCreate()");
    SDL_ENFORCE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER));
    g_window = SDL_CreateWindow( "SDL Synth", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 300, 200, SDL_WINDOW_SHOWN);
    SDL_ENFORCE_PTR(g_window);
    g_renderer = SDL_CreateRenderer(g_window, -1, 0);
    SDL_ENFORCE_PTR(g_window);
    SDL_ENFORCE(SDL_SetRenderDrawColor(g_renderer, 255, 255, 0, 255));
    SDL_ENFORCE(SDL_RenderClear(g_renderer));
    SDL_RenderPresent(g_renderer);
}

#define KEYS_NUM 16
const char *g_keys = "zsxcfvgbnjmk\xbcl\xbe\xbf";

void synth_appHandleKey(SDL_Keycode sym, bool pressed, FTYPE time)
{
    for (int k = 0; k < KEYS_NUM; k++)
    {
        char key = g_keys[k];
        if (sym != key) {
            continue;
        }


        // Check if note already exists in currently playing notes
        //muxNotes.lock();
        //auto noteFound = find_if(vecNotes.begin(), vecNotes.end(), [&k](synth::note const& item) { return item.id == k; });


        struct synth_Note *note = NULL;
        for (int i = 0; i < NOTES_NUM; ++i) {
            note = g_notes[i];
            if (note != NULL && note->id == k) {
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

                        g_notes[i]->id = k;
                        g_notes[i]->on = time;
                        g_notes[i]->channel = 1;
                        g_notes[i]->active = true;

                        break;
                    }
                }
            }
            else
            {
                // Note not in vector, but key has been released...
                // ...nothing to do
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
        const FTYPE finish = synth_appGetTime();
        const FTYPE sleep = TICK_TIME - (finish - start);
        if (sleep > 0) {
            synth_appSleep(sleep);
        }
        last = start;
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
