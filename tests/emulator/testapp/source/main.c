// Test "game" for BinguRecorder in an emulator.
//
// Top screen: moving gradient, a white square sliding across, the frame
// number as 16 black/white blocks (so a recording can be checked frame by
// frame), a second row with the time since start in 10 ms steps (so sound
// and picture can be compared even when the frame rate changes) and a
// corner that flashes white for 3 frames every second.
// Bottom screen: a different moving pattern with the same frame number.
// Sound: a quiet 330 Hz tone, plus a loud 1 kHz beep for 0.1 s every second,
// at the same moment as the flash.
#include <3ds.h>
#include <math.h>
#include <string.h>

#define TOP_W 400
#define BOTTOM_W 320
#define H 240
#define RATE 32000
#define WAVE_SAMPLES 1600   // 50 ms per buffer
#define WAVE_COUNT 4

u32 NnStylePresent(u32 isBottom, int bank, void *leftFb, void *rightFb, int stride, int format, int swap);
Result NnStyleRegisterInterruptRelayQueue(Handle *session, Handle event, u32 flags, Handle *outSharedMem, u32 *outThreadId);

u32 PresentImpl(u32 isBottom, int bank, void *leftFb, void *rightFb, int stride, int format, int swap)
{
    (void)swap;
    gspPresentBuffer(isBottom ? GFX_BOTTOM : GFX_TOP, bank, leftFb, rightFb, stride, format);
    return 0;
}

static inline void Put(u8 *fb, int x, int y, u8 r, u8 g, u8 b)
{
    u8 *p = fb + (x * H + (H - 1 - y)) * 3;
    p[0] = b; p[1] = g; p[2] = r;
}

static void Rect(u8 *fb, int x0, int y0, int w, int h, u8 r, u8 g, u8 b)
{
    for (int x = x0; x < x0 + w; ++x)
        for (int y = y0; y < y0 + h; ++y)
            Put(fb, x, y, r, g, b);
}

// 16 blocks of 20x20: white = 1, black = 0, bit 0 on the left
static void DrawBits(u8 *fb, int y, u32 n)
{
    Rect(fb, 0, y, 16 * 24 + 4, 28, 255, 0, 255);
    for (int i = 0; i < 16; ++i)
    {
        u8 v = (n >> i) & 1 ? 255 : 0;
        Rect(fb, 4 + i * 24, y + 4, 20, 20, v, v, v);
    }
}

static void DrawCounter(u8 *fb, u32 n)
{
    DrawBits(fb, 0, n);
}

static void DrawTop(u8 *fb, u32 n, u32 centiseconds, bool flash)
{
    for (int x = 0; x < TOP_W; ++x)
    {
        u8 r = (x + n * 2) & 0xFF, g = 64, b = 255 - ((x + n) & 0xFF);
        for (int y = 28; y < H; ++y)
            Put(fb, x, y, r, (u8)(g + y / 2), b);
    }
    DrawCounter(fb, n);
    DrawBits(fb, 28, centiseconds);
    Rect(fb, (n * 4) % (TOP_W - 40), 100, 40, 40, 255, 255, 255);
    Rect(fb, 0, H - 40, 40, 40, flash ? 255 : 0, flash ? 255 : 0, flash ? 255 : 0);
}

static void DrawBottom(u8 *fb, u32 n)
{
    for (int x = 0; x < BOTTOM_W; ++x)
        for (int y = 28; y < H; ++y)
            Put(fb, x, y, 0, (u8)(x / 2), 96);
    DrawCounter(fb, n);
    Rect(fb, 140, 28 + (n * 3) % (H - 28 - 40), 40, 40, 255, 32, 32);
}

int main(void)
{
    gfxInitDefault();

    // Games register for GPU interrupts through Nintendo's library code,
    // which CTRPF hooks. Do the same on a second GSP session.
    Handle gspSession = 0, gspEvent = 0, gspSharedMem = 0;
    u32 gspThreadId = 0;
    if (R_SUCCEEDED(srvGetServiceHandle(&gspSession, "gsp::Gpu"))
        && R_SUCCEEDED(svcCreateEvent(&gspEvent, RESET_ONESHOT)))
        NnStyleRegisterInterruptRelayQueue(&gspSession, gspEvent, 1, &gspSharedMem, &gspThreadId);

    // Own framebuffers, presented through NnStylePresent
    u8 *top[2], *bottom[2];
    for (int i = 0; i < 2; ++i)
    {
        top[i] = linearAlloc(TOP_W * H * 3);
        bottom[i] = linearAlloc(BOTTOM_W * H * 3);
    }

    // Sound (needs sdmc:/3ds/dspfirm.cdc; the emulator accepts any file)
    bool sound = R_SUCCEEDED(ndspInit());
    ndspWaveBuf wave[WAVE_COUNT];
    s16 *waveData = NULL;
    u64 sample = 0;

    if (sound)
    {
        float mix[12] = { 1.0f, 1.0f };
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, RATE);
        ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
        ndspChnSetMix(0, mix);

        waveData = linearAlloc(WAVE_SAMPLES * WAVE_COUNT * sizeof(s16));
        memset(wave, 0, sizeof(wave));
        for (int i = 0; i < WAVE_COUNT; ++i)
        {
            wave[i].data_pcm16 = waveData + i * WAVE_SAMPLES;
            wave[i].status = NDSP_WBUF_DONE;
        }
    }

    const u64 start = svcGetSystemTick();
    u32 n = 0;
    int bank = 0;

    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;

        // Keep the sound queue filled; the beep is tied to the sample clock
        for (int i = 0; sound && i < WAVE_COUNT; ++i)
        {
            if (wave[i].status != NDSP_WBUF_DONE)
                continue;

            s16 *out = (s16 *)wave[i].data_pcm16;
            for (int s = 0; s < WAVE_SAMPLES; ++s, ++sample)
            {
                double t = (double)sample / RATE;
                bool beep = (sample % RATE) < RATE / 10;
                out[s] = (s16)(3000 * sin(2 * M_PI * 330 * t) + (beep ? 16000 * sin(2 * M_PI * 1000 * t) : 0));
            }
            wave[i].nsamples = WAVE_SAMPLES;
            DSP_FlushDataCache(out, WAVE_SAMPLES * sizeof(s16));
            ndspChnWaveBufAdd(0, &wave[i]);
        }

        // Flash for 3 frames at every full second
        double seconds = (double)(svcGetSystemTick() - start) / SYSCLOCK_ARM11;
        bool flash = seconds - (u32)seconds < 3.0 / 60;

        bank ^= 1;
        DrawTop(top[bank], n, (u32)(seconds * 100), flash);
        DrawBottom(bottom[bank], n);
        GSPGPU_FlushDataCache(top[bank], TOP_W * H * 3);
        GSPGPU_FlushDataCache(bottom[bank], BOTTOM_W * H * 3);

        NnStylePresent(0, bank, top[bank], top[bank], H * 3, GSP_BGR8_OES | BIT(6) | (1 << 8), 0);
        NnStylePresent(1, bank, bottom[bank], bottom[bank], H * 3, GSP_BGR8_OES | (1 << 8), 0);

        gspWaitForVBlank();
        ++n;
    }

    if (sound)
        ndspExit();
    gfxExit();
    return 0;
}
