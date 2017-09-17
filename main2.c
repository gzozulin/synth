#include <assert.h>
#include <stdbool.h>
#include "c11threads.h"

#include "SDL2/SDL.h"

// -------------------------- +Const --------------------------

#define       FREQUENCY             44100
#define       SAMPLES               512

#define       TICK_TIME             (1.0f / 60.0f)

#define       SAMPLE_TIME           (1.0f / (float) FREQUENCY)

#define       AUDIO_DEV_ID          1

// -------------------------- +Variables --------------------------

char          g_logBuffer[1024];

SDL_Window    *g_window             = NULL;
SDL_Renderer  *g_renderer           = NULL;

bool          g_quit                = false;

float         g_audioBuffer[2048];

// -------------------------- +Common --------------------------

typedef float output;

extern inline output synth_appGetTime()
{
    return (output) SDL_GetTicks() / 1000.0f;
}

extern inline void  synth_appSleep(output seconds)
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

// -------------------------- +Audio --------------------------

void synth_audioAppendBuffer(SDL_AudioDeviceID dev, output start, output *accumulator)
{
    uint index = 0;
    while (*accumulator > SAMPLE_TIME) {
        const output time = start + index * SAMPLE_TIME;
        const output sample = sinf(time);
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

void synth_appHandleKeyDown(SDL_Keycode sym, output time)
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

void synth_appHandleKeyUp(SDL_Keycode sym, output time)
{
    switch (sym) {
        case ' ': {
            break;
        }
        default:break;
    }
}

void synth_appPollEvents(output time)
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

void synth_appRunLoop()
{
    logi("synth_appRunLoop() called");
    output accumulator = 0.0f;
    output last = synth_appGetTime();
    while (!g_quit) {
        const output start = synth_appGetTime();
        synth_appPollEvents(start);
        const output elapsed = start - last;
        accumulator += elapsed;
        synth_audioAppendBuffer(AUDIO_DEV_ID, last, &accumulator);
        const output finish = synth_appGetTime();
        const output sleep = TICK_TIME - (finish - start);
        if (sleep > 0) {
            synth_appSleep(sleep);
        }
        last = start;
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
