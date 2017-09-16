#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include <assert.h>
#include <float.h>

#include "c11threads.h"

#include "SDL2/SDL.h"

// -------------------------- +Const --------------------------

//#define       TESTS

#define       FREQUENCY             44100
#define       SAMPLES               512
#define       SAMPLE_TIME           (1.0f / (float) FREQUENCY)

SDL_Window    *g_window             = NULL;
SDL_Renderer  *g_renderer           = NULL;

bool          g_quit                = false;

#define       ONE_TICK              (1.0f / 60.0f)
#define       SAMPLES_FOR_TICK      (ONE_TICK / SAMPLE_TIME)

#define       F_PI                  ((float) M_PI)

#define       MASTER_VOLUME         0.4f

#define       AUDIO_DEV_ID          1

// -------------------------- +Macroses --------------------------

extern inline float     synth_appGetTime()
{
    return (float) SDL_GetTicks() / 1000.0f;
}

/*extern inline void      synth_appSleep(float seconds)
{
    const long millis = (const long) (seconds * 1e+6);
    const long nanos = millis * 1000L;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = nanos };
    thrd_sleep(&ts, NULL);
}*/

extern inline float     synth_convertFrequency(float hertz) { return hertz * 2.0f * F_PI; }

extern inline float     synth_calculateFrequency(int note) // octave??
{
    const float baseFrequency = 110.0f; // A2
    const float twelwthRootOf2 = powf(2.0f, 1.0f / 12.0f);
    return baseFrequency * powf(twelwthRootOf2, note);
}

#define logfmt(fmt, ...) printf(fmt, __VA_ARGS__)
#define logi(what) { printf("%.2f -> INFO -> %s:%d %s\n", synth_appGetTime(), __FILE__, __LINE__, what); }
#define loge(what) { printf("%.2f -> ERROR -> %s:%d %s\n", synth_appGetTime(), __FILE__, __LINE__, what); exit(-1); }

#define SDL_FAIL() { printf("%.2f -> SDL_ERROR -> %s:%d -> %s\n", synth_appGetTime(), __FILE__, __LINE__, SDL_GetError()); exit(-1); }
#define SDL_ENFORCE(expr) { if ((expr) < 0)  SDL_FAIL(); }
#define SDL_ENFORCE_PTR(ptr) { if ((ptr) == NULL) SDL_FAIL(); }

// -------------------------- +Oscillator --------------------------

int         g_baseFrequencyIndex    = 0;
float       g_baseFrequency         = 110.0f;
#define     BASE_FREQUENCIES_NUM    10

void synth_increaseBaseFrequency()
{
    g_baseFrequencyIndex++;
    g_baseFrequencyIndex %= BASE_FREQUENCIES_NUM;
    g_baseFrequency = synth_calculateFrequency(g_baseFrequencyIndex);
    logfmt("Base frequency increased %f\n", g_baseFrequency);
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

float synth_oscillate(enum synth_WaveType type, float frequency, float time)
{
    switch (type) {
        case WAVE_TYPE_SINE: {
            return sinf(synth_convertFrequency(frequency) * time);
        }
        case WAVE_TYPE_SQUARE: {
            return sinf(synth_convertFrequency(frequency) * time) > 0.0f ? 1.0f : -1.0f;
        }
        case WAVE_TYPE_TRIANGLE: {
            return asinf(sinf(synth_convertFrequency(frequency) * time)) * (2.0f / F_PI);
        }
        case WAVE_TYPE_SAW_ANALOGUE: {
            float output = 0.0f;
            for (int n = 1; n < 100; n++) {
                output += (sinf((float) n * synth_convertFrequency(frequency) * time)) / (float) n;
            }
            return output * (2.0f / F_PI);
        }
        case WAVE_TYPE_SAW_DIGITAL: {
            return  (2.0f / F_PI) * (frequency * F_PI * fmodf(time, 1.0f / frequency) - (F_PI / 2.0f));
        }
        case WAVE_TYPE_NOISE: {
            return 2.0f * ((float) random() / (float) RAND_MAX) - 1.0f;
        }
    }
    loge("Unknown function type");
    return 0.0f;
}

struct synth_Envelope
{
    float attackTime;
    float decayTime;
    float releaseTime;

    float startAmplitude;
    float sustainAmplitude;

    float onTimestamp;
    float offTimestamp;

    bool noteOn;
};

struct synth_Envelope g_envelope = { 0.01f, 0.1f, 0.2f, 1.0f, 0.7f, 0L, 0L, false };

void synth_envelopeNoteOn(struct synth_Envelope *envelope, float time)
{
    assert(envelope != 0);
    envelope->noteOn = true;
    envelope->onTimestamp = time;
}

void synth_envelopeNoteOff(struct synth_Envelope *envelope, float time)
{
    assert(envelope != 0);
    envelope->noteOn = false;
    envelope->offTimestamp = time;
}

float synth_envelopeGetAmplitude(struct synth_Envelope *envelope, float time)
{
    assert(envelope != 0);
    float amplitude;
    float lifetime = time - envelope->onTimestamp;
    if (envelope->noteOn) {
        if (lifetime <= envelope->attackTime) {
            amplitude = (lifetime / envelope->attackTime) * envelope->startAmplitude;
        } else if (lifetime <= (envelope->attackTime + envelope->decayTime)) {
            amplitude = ((lifetime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;
        } else {
            amplitude = envelope->sustainAmplitude;
        }
    } else {
        amplitude = ((time - envelope->offTimestamp) / envelope->releaseTime) * (0.0f - envelope->sustainAmplitude) + envelope->sustainAmplitude;
    }
    if (amplitude <= FLT_EPSILON) {
        amplitude = 0.0f;
    }
    return amplitude;
}

float synth_oscCreateSample(struct synth_Envelope *envelope, float time)
{
    assert(envelope != NULL);
    return MASTER_VOLUME * synth_envelopeGetAmplitude(envelope, time) *
            (synth_oscillate(WAVE_TYPE_SAW_ANALOGUE , g_baseFrequency, time) + synth_oscillate(WAVE_TYPE_SINE , g_baseFrequency * 0.5f, time));
}

// -------------------------- +Audio --------------------------

float *g_audioBuffer = NULL;

void synth_audioAppendBufferForOneTick(SDL_AudioDeviceID dev, float start)
{
    const uint size = (int) SAMPLES_FOR_TICK * sizeof(float);
    if (g_audioBuffer == NULL) {
        g_audioBuffer = malloc(size);
    }
    for (int s = 0; s < SAMPLES_FOR_TICK; ++s) {
        const float time = start + s * SAMPLE_TIME;
        const float sample = synth_oscCreateSample(&g_envelope, time);
        //logfmt("%f: sample #%d -> %f\n", time, g_ringBufferWriteCursor, sample);
        g_audioBuffer[s] = sample;
    }
    SDL_ENFORCE(SDL_QueueAudio(dev, g_audioBuffer, size));
}

void synth_audioDeviceList()
{
    logi("synth_audioDeviceList()");
    const int num = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < num; ++i) {
        logfmt("Found audio device: %s\n", SDL_GetAudioDeviceName(i, 0));
    }
}

void synth_audioDevicePrintSpec(SDL_AudioSpec *spec)
{
    assert(spec != NULL);
    logfmt("Received freq: %d\n", spec->freq);
    logfmt("Received format: %d\n", spec->format);
    logfmt("Received channels: %d\n", spec->channels);
    logfmt("Received samples: %d\n", spec->samples);
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
    logi("Asked:\n")
    synth_audioDevicePrintSpec(&asked);
    logi("Received:\n")
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

void synth_appHandleKeyDown(SDL_Keycode sym, float time)
{
    switch (sym) {
        case ' ': {
            if (!g_envelope.noteOn) {
                synth_envelopeNoteOn(&g_envelope, time);
            }
            break;
        }
        default: {
            synth_increaseBaseFrequency();
            break;
        };
    }
}

void synth_appHandleKeyUp(SDL_Keycode sym, float time)
{
    switch (sym) {
        case ' ': {
            synth_envelopeNoteOff(&g_envelope, time);
            break;
        }
        default:break;
    }
}

void synth_appPollEvents(float time)
{
    SDL_Event event;
    while( SDL_PollEvent(&event) != 0 ) {
        if(event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == 27)) {
            g_quit = true;
        } else if (event.type == SDL_KEYDOWN) {
            synth_appHandleKeyDown(event.key.keysym.sym, time);
        } else if (event.type == SDL_KEYUP) {
            synth_appHandleKeyUp(event.key.keysym.sym, time);
        }
    }
}

void synth_appUpdateFps()
{
    static char title[32];
    static int fps = 0;
    static int lastSecond = 0;
    fps++;
    const int second = (const int) synth_appGetTime();
    if (lastSecond != second) {
        sprintf(title, "Synthesizer, fps: %d", fps);
        SDL_SetWindowTitle(g_window, title);
        lastSecond = second;
        fps = 0;
    }
}

void synth_appRunLoop()
{
    logi("synth_appRunLoop() called");
    float accumulator = 0.0f;
    float last = synth_appGetTime();
    while (!g_quit) {
        const float current = synth_appGetTime();
        synth_appPollEvents(current);
        const float elapsed = current - last;
        accumulator += elapsed;
        int tick = 0;
        while (accumulator  >= ONE_TICK) {
            synth_appUpdateFps();
            synth_audioAppendBufferForOneTick(AUDIO_DEV_ID, last + tick * ONE_TICK);
            accumulator -= ONE_TICK;
            tick++;
        }
        last = current;
    }
}

// -------------------------- +Main --------------------------

int main()
{
#ifdef TESTS
    synth_ringBufferClear();
    synth_ringBufferTests();
    return 0;
#else
    synth_appWinCreate();
    synth_audioDevicePrepare();
    SDL_PauseAudio(0);
    synth_appRunLoop();
    SDL_CloseAudio();
    SDL_Quit();
    return 0;
#endif
}