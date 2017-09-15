#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include <assert.h>
#include <float.h>

#include "SDL2/SDL.h"

// -------------------------- +Const --------------------------

//#define       TESTS

#define       FREQUENCY             44100
#define       SAMPLES               1024
#define       SAMPLE_TIME           (1000.0 / (double) FREQUENCY)

SDL_Window    *g_window             = NULL;
SDL_Renderer  *g_renderer           = NULL;

bool          g_quit                = false;

#define       KEYS_NUM              7
const char    g_keys[]              = "zxcvbnm";

#define       TIMESTAMP_NOT_SET     (-1L)

#define       ENVELOPES_NUM         7

// -------------------------- +Macroses --------------------------

#define logfmt(fmt, ...) printf(fmt, __VA_ARGS__)
#define logi(what) { printf("INFO -> %s:%d %s\n", __FILE__, __LINE__, what); }
#define loge(what) { printf("ERROR -> %s:%d %s\n", __FILE__, __LINE__, what); exit(-1); }
#undef  printf // todo: not working

#define SDL_FAIL() { printf("SDL_ERROR -> %s:%d -> %s\n", __FILE__, __LINE__, SDL_GetError()); exit(-1); }
#define SDL_ENFORCE(expr) { if ((expr) < 0)  SDL_FAIL(); }
#define SDL_ENFORCE_PTR(ptr) { if ((ptr) == NULL) SDL_FAIL(); }

extern inline int       synth_min(const int x, const int y) { return y < x ? y : x; }
extern inline double    synth_convertFrequency(double herz) { return herz * 2.0 * M_PI; }
extern inline short     synth_convertWave(double wave) { return (short) (SHRT_MAX * wave); }

extern inline double    synth_calculateFrequency(int key)
{
    const double baseFrequency = 110.0; // A2
    const double twelwthRootOf2 = pow(2.0, 1.0 / 12.0);
    return baseFrequency * pow(twelwthRootOf2, key);
}

// -------------------------- +RingBuffer --------------------------

#define RING_BUFFER_SIZE 100
short g_ringBuffer[RING_BUFFER_SIZE];

uint g_ringBufferWriteCursor = 0;
uint g_ringBufferReadCursor = 0;

void synth_ringBufferClear()
{
    memset(g_ringBuffer, 0, RING_BUFFER_SIZE * sizeof(short));
}

void synth_ringBufferWriteOne(short value)
{
    g_ringBuffer[g_ringBufferWriteCursor] = value;
    g_ringBufferWriteCursor++;
    g_ringBufferWriteCursor %= RING_BUFFER_SIZE;
}

void synth_ringBufferReadMany(short *dest, int num)
{
    assert(dest != 0);
    const int left = RING_BUFFER_SIZE - g_ringBufferReadCursor;
    const int before = synth_min(num, left);
    const int after = num - before;
    memcpy(dest, g_ringBuffer + g_ringBufferReadCursor, before * sizeof(short));
    g_ringBufferReadCursor += before;
    g_ringBufferReadCursor %= RING_BUFFER_SIZE;
    if (after > 0) {
        synth_ringBufferReadMany(dest + before, after);
    }
}

#ifdef TESTS
void synth_ringBufferTests()
{
    short dest[2048];
    memset(g_ringBuffer, 0, sizeof(short) * RING_BUFFER_SIZE);
    g_ringBufferReadCursor = 75;
    synth_ringBufferReadMany(dest, 125);
    synth_ringBufferReadMany(dest, 25);
    assert(g_ringBufferReadCursor == 25);
}
#endif

// -------------------------- +Oscillator --------------------------

enum synth_WaveType
{
    WAVE_TYPE_SIN,
    WAVE_TYPE_SQUARE,
    WAVE_TYPE_TRIANGLE,
    WAVE_TYPE_SAW_ANALOGUE,
    WAVE_TYPE_SAW_OPTIMIZED,
    WAVE_TYPE_NOISE
};

struct synth_Envelope
{
    enum synth_WaveType waveType;

    double attackTime;
    double decayTime;
    double releaseTime;

    double startAmplitude;
    double sustainAmplitude;

    double triggerOnTimestamp;
    double triggerOffTimestamp;
};

struct synth_Envelope g_envelopes[ENVELOPES_NUM] =
{
        { WAVE_TYPE_TRIANGLE, 100, 100, 200, 1.0, 0.8, TIMESTAMP_NOT_SET, TIMESTAMP_NOT_SET },
        { WAVE_TYPE_TRIANGLE, 100, 100, 200, 1.0, 0.8, TIMESTAMP_NOT_SET, TIMESTAMP_NOT_SET },
        { WAVE_TYPE_TRIANGLE, 100, 100, 200, 1.0, 0.8, TIMESTAMP_NOT_SET, TIMESTAMP_NOT_SET },
        { WAVE_TYPE_TRIANGLE, 100, 100, 200, 1.0, 0.8, TIMESTAMP_NOT_SET, TIMESTAMP_NOT_SET },
        { WAVE_TYPE_TRIANGLE, 100, 100, 200, 1.0, 0.8, TIMESTAMP_NOT_SET, TIMESTAMP_NOT_SET },
        { WAVE_TYPE_TRIANGLE, 100, 100, 200, 1.0, 0.8, TIMESTAMP_NOT_SET, TIMESTAMP_NOT_SET },
        { WAVE_TYPE_TRIANGLE, 100, 100, 200, 1.0, 0.8, TIMESTAMP_NOT_SET, TIMESTAMP_NOT_SET }
};

double synth_oscillate(enum synth_WaveType type, double frequency, double dt)
{
    switch (type) {
        case WAVE_TYPE_SIN: {
            return sin(synth_convertFrequency(frequency) * dt);
        }
        case WAVE_TYPE_SQUARE: {
            return sin(synth_convertFrequency(frequency) * dt) > 0.0 ? 1.0 : -1.0;
        }
        case WAVE_TYPE_TRIANGLE: {
            return asin(sin(synth_convertFrequency(frequency) * dt) * 2.0 / M_PI);
        }
        case WAVE_TYPE_SAW_ANALOGUE: {
            double output = 0.0;
            for (int n = 1; n < 100; n++) {
                output += (sin((double) n * synth_convertFrequency(frequency) * dt)) / (double) n;
            }
            return output * (2.0 / M_PI);
        }
        case WAVE_TYPE_SAW_OPTIMIZED: {
            return (2.0 / M_PI) * (frequency * M_PI * fmod(dt, 1.0 / frequency) - (M_PI / 2.0));
        }
        case WAVE_TYPE_NOISE: {
            return 2.0 * ((double) rand() / (double) RAND_MAX) - 1.0;
        }
    }
    loge("Unknown function type");
    return 0.0;
}

double synth_envelopeCreateAmplitude(struct synth_Envelope *envelope, double time)
{
    assert(envelope != 0);
    double amplitude = 0.0;
    if (envelope->triggerOnTimestamp == TIMESTAMP_NOT_SET) {
        return amplitude;
    }
    if (envelope->triggerOffTimestamp == TIMESTAMP_NOT_SET) { // note on
        const double lifetime = time - envelope->triggerOnTimestamp;
        assert(lifetime >= 0.0);
        if (lifetime <= envelope->attackTime) { // A
            logi("attack");
            amplitude = (lifetime / envelope->attackTime) * envelope->startAmplitude;
        } else if (lifetime <= (envelope->attackTime + envelope->decayTime)) { // D
            logi("decay");
            amplitude = ((lifetime - envelope->attackTime) / envelope->decayTime) * (envelope->sustainAmplitude - envelope->startAmplitude) + envelope->startAmplitude;
        } else { // S
            logi("sustain");
            amplitude = envelope->sustainAmplitude;
        }
    } else { // note off, R
        logi("release");
        amplitude = ((time - envelope->triggerOffTimestamp) / envelope->releaseTime) * (0.0 - envelope->sustainAmplitude) + envelope->sustainAmplitude;
    }
    if (amplitude < DBL_EPSILON) {
        amplitude = 0.0;
    }
    return amplitude;
}

short synth_oscCreateNoise(struct synth_Envelope *envelope, double volume, double frequency, double sampleTime, double time)
{
    return synth_convertWave(
            volume * synth_envelopeCreateAmplitude(envelope, time) * synth_oscillate(envelope->waveType, frequency, sampleTime)
    );
}

void synth_updateBuffer(double startTime, double dt)
{
    double passed = 0.0;
    while (passed < dt) {
        struct synth_Envelope *envelope = &g_envelopes[0];
        synth_ringBufferWriteOne(synth_oscCreateNoise(envelope, 1.0, 220.0 /*???????*/, SAMPLE_TIME, startTime + passed));
        passed += SAMPLE_TIME;
    }
}

// -------------------------- +Audio --------------------------

void synth_audioDeviceCallback(void *userData, Uint8 *data, int length)
{
    synth_ringBufferReadMany((short *) data, SAMPLES);
}

void synth_audioDeviceList()
{
    logi("synth_audioDeviceList()");
    const int num = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < num; ++i) {
        logfmt("Found audio device: %s\n", SDL_GetAudioDeviceName(i, 0));
    }
}

void synth_audioDevicePrepare()
{
    logi("synth_audioDevicePrepare()");
    synth_audioDeviceList();
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = FREQUENCY;
    want.format = AUDIO_S16;
    want.channels = 1;
    want.samples = SAMPLES;
    want.callback = synth_audioDeviceCallback;
    SDL_ENFORCE(SDL_OpenAudio(&want, 0));
}

// -------------------------- +Application --------------------------

double synth_currentTime()
{
    return (double) SDL_GetTicks() / 1000.0;
}

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

void synth_appPollEvents(double currentUpdate)
{
    SDL_Event event;
    while( SDL_PollEvent(&event) != 0 ) {
        if(event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == 27)) {
            g_quit = true;
        } else if (event.type == SDL_KEYDOWN) {
            g_envelopes[0].triggerOnTimestamp = currentUpdate;
            g_envelopes[0].triggerOffTimestamp = TIMESTAMP_NOT_SET;
        } else if (event.type == SDL_KEYUP) {
            g_envelopes[0].triggerOffTimestamp = currentUpdate;
        }
    }
}

void synth_appRunLoop()
{
    logi("synth_appRunLoop()");
    double lastUpdate = synth_currentTime();
    while (!g_quit) {
        const double currentUpdate = synth_currentTime();
        synth_appPollEvents(currentUpdate);
        synth_updateBuffer(lastUpdate, currentUpdate - lastUpdate);
        lastUpdate = currentUpdate;
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
    logi("main()");
    synth_ringBufferClear();
    synth_appWinCreate();
    synth_audioDevicePrepare();
    SDL_PauseAudio(0);
    synth_appRunLoop();
    SDL_CloseAudio();
    SDL_Quit();
    return 0;
#endif
}