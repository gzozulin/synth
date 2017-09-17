#include <assert.h>
#include <stdbool.h>
#include "c11threads.h"

#include "SDL2/SDL.h"

// -------------------------- +Const --------------------------

typedef float synth_Output;

#define       FREQUENCY             44100
#define       SAMPLES               512

#define       TICK_TIME             (1.0f / 60.0f)

#define       SAMPLE_TIME           (1.0f / (synth_Output) FREQUENCY)

#define       AUDIO_DEV_ID          1

#define       PI                    ((float) M_PI)

// -------------------------- +Variables --------------------------

char          g_logBuffer[1024];

SDL_Window    *g_window             = NULL;
SDL_Renderer  *g_renderer           = NULL;

bool          g_quit                = false;

synth_Output        g_audioBuffer[2048];

// -------------------------- +Common --------------------------

extern inline synth_Output synth_appGetTime()
{
    return (synth_Output) SDL_GetTicks() / 1000.0f;
}

extern inline void  synth_appSleep(synth_Output seconds)
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

extern inline float synth_convertFrequency(float hertz) { return hertz * 2.0f * PI; }

struct synth_Note
{
    int id;
    synth_Output on;
    synth_Output off;
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
            return asinf(sinf(synth_convertFrequency(frequency) * time)) * (2.0f / PI);
        }
        case WAVE_TYPE_SAW_ANALOGUE: {
            float output = 0.0f;
            for (int n = 1; n < 100; n++) {
                output += (sinf((float) n * synth_convertFrequency(frequency) * time)) / (float) n;
            }
            return output * (2.0f / PI);
        }
        case WAVE_TYPE_SAW_DIGITAL: {
            return  (2.0f / PI) * (frequency * PI * fmodf(time, 1.0f / frequency) - (PI / 2.0f));
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
    synth_Output attackTime;
    synth_Output decayTime;
    synth_Output startAmplitude;
    synth_Output sustainAmplitude;
    synth_Output releaseTime;
};

float synth_envelopeGetAmplitude(struct synth_Envelope *envelope, const synth_Output time, const synth_Output timeOn, const synth_Output timeOff)
{
    assert(envelope != NULL);
    
    synth_Output dAmplitude = 0.0;
    synth_Output dReleaseAmplitude = 0.0;

    if (timeOn > timeOff) // Note is on
    {
        synth_Output dLifeTime = time - timeOn;

        if (dLifeTime <= envelope->attackTime)
            dAmplitude = (dLifeTime / envelope->attackTime) * envelope->startAmplitude;

        if (dLifeTime > envelope->attackTime && dLifeTime <= (envelope->attackTime + envelope->decayTime))
            dAmplitude = ((dLifeTime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;

        if (dLifeTime > (envelope->attackTime + envelope->decayTime))
            dAmplitude = envelope->sustainAmplitude;
    }
    else // Note is off
    {
        synth_Output dLifeTime = timeOff - timeOn;

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

typedef synth_Output(*synth_VoicePtr)(struct synth_Envelope *, float, float, float);

struct synth_Imstrument
{
    synth_Output volume;
    struct synth_Envelope envelope;
    synth_VoicePtr voice;
};

// -------------------------- +Audio --------------------------

void synth_audioAppendBuffer(SDL_AudioDeviceID dev, synth_Output start, synth_Output *accumulator)
{
    uint index = 0;
    while (*accumulator > SAMPLE_TIME) {
        const synth_Output time = start + index * SAMPLE_TIME;
        const synth_Output sample = sinf(time);
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

void synth_appHandleKey(SDL_Keycode sym, bool pressed, synth_Output time)
{
    switch (sym) {
        case ' ': {

            break;
        }
        default: {
            break;
        };
    }
}

void synth_appPollEvents(synth_Output time)
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
    synth_Output accumulator = 0.0f;
    synth_Output last = synth_appGetTime();
    while (!g_quit) {
        const synth_Output start = synth_appGetTime();
        synth_appPollEvents(start);
        const synth_Output elapsed = start - last;
        accumulator += elapsed;
        synth_audioAppendBuffer(AUDIO_DEV_ID, last, &accumulator);
        const synth_Output finish = synth_appGetTime();
        const synth_Output sleep = TICK_TIME - (finish - start);
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
