// Host-side test for BinguPlayer's file reading, audio decoding and screen
// mapping (everything except the 3DS-specific parts).
//
// usage: playertest info <file.avi>
//        playertest audio <file.avi> <out.pcm>     decoded audio, s16le
//        playertest frame0 <file.avi> <out.rgb>    first raw frame as RGB24
//        playertest blit                           screen mapping self-test
#include "AviReader.hpp"
#include "FrameBlit.hpp"
#include "ImaAdpcmDecoder.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace Bingu;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAILED: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

static int Info(const char *path)
{
    AviReader avi;
    std::string error = avi.Open(path);
    if (!error.empty())
    {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    uint32_t real = 0;
    for (uint32_t i = 0; i < avi.FrameCount(); ++i)
        real += avi.Frame(i).size != 0;

    printf("width=%u height=%u codec=%s fps=%.3f frames=%u real=%u last_real_12=%d",
        avi.Width(), avi.Height(), avi.Codec() == AviReader::VideoCodec::Mjpeg ? "mjpeg" : "rgb555",
        avi.Fps(), avi.FrameCount(), real, avi.LastRealFrame(12));
    if (avi.HasAudio())
        printf(" audio=%u/%u/%u/%u chunks=%u", avi.SampleRate(), avi.Channels(), avi.BlockAlign(),
            avi.SamplesPerBlock(), avi.AudioChunkCount());
    printf("\n");

    // Every MJPEG frame must be a complete JPEG
    if (avi.Codec() == AviReader::VideoCodec::Mjpeg)
    {
        std::vector<uint8_t> buf(avi.MaxVideoChunk());
        for (uint32_t i = 0; i < avi.FrameCount(); ++i)
        {
            const AviReader::Chunk &c = avi.Frame(i);
            if (!c.size)
                continue;
            CHECK(avi.Read(c, buf.data()));
            CHECK(buf[0] == 0xFF && buf[1] == 0xD8 && buf[c.size - 2] == 0xFF && buf[c.size - 1] == 0xD9);
        }
    }

    // Seeking: the chunk found for a sample must contain it
    if (avi.HasAudio() && avi.AudioChunkCount() > 2)
    {
        const AviReader::AudioChunk &second = avi.Audio(1);
        CHECK(avi.AudioChunkAt(0) == 0);
        CHECK(avi.AudioChunkAt(second.firstSample) == 1);
        CHECK(avi.AudioChunkAt(second.firstSample - 1) == 0);
        CHECK(avi.AudioChunkAt(0xFFFFFFFF) == avi.AudioChunkCount() - 1);
    }
    return 0;
}

static int Audio(const char *path, const char *out)
{
    AviReader avi;
    CHECK(avi.Open(path).empty() && avi.HasAudio());

    FILE *f = fopen(out, "wb");
    CHECK(f);
    std::vector<uint8_t> chunk(avi.MaxAudioChunk());
    std::vector<int16_t> pcm(avi.SamplesPerBlock() * avi.Channels());

    for (uint32_t i = 0; i < avi.AudioChunkCount(); ++i)
    {
        const AviReader::AudioChunk &c = avi.Audio(i);
        CHECK(avi.Read(c, chunk.data()));
        for (uint32_t b = 0; b < c.size / avi.BlockAlign(); ++b)
        {
            DecodeImaAdpcmBlock(chunk.data() + b * avi.BlockAlign(), avi.BlockAlign(), avi.Channels(),
                avi.SamplesPerBlock(), pcm.data());
            fwrite(pcm.data(), 2, pcm.size(), f);
        }
    }
    fclose(f);
    return 0;
}

static int Frame0(const char *path, const char *out)
{
    AviReader avi;
    CHECK(avi.Open(path).empty() && avi.Codec() == AviReader::VideoCodec::Rgb555);

    std::vector<uint16_t> raw(avi.Width() * avi.Height());
    std::vector<uint8_t> bgr(avi.Width() * avi.Height() * 3);
    CHECK(avi.Read(avi.Frame(0), raw.data()));
    Rgb555ToBgr24(raw.data(), avi.Width(), avi.Height(), bgr.data());

    FILE *f = fopen(out, "wb");
    CHECK(f);
    for (size_t i = 0; i < bgr.size(); i += 3)
    {
        uint8_t rgb[3] = { bgr[i + 2], bgr[i + 1], bgr[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

// Screen pixel (x, y), y top-down, of a rotated framebuffer
static const uint8_t *FbPixel(const std::vector<uint8_t> &fb, uint32_t x, uint32_t y)
{
    return &fb[(x * 240 + (239 - y)) * 3];
}

static int Blit(void)
{
    // Layout decisions for every recording mode
    Layout l = ChooseLayout(400, 240);
    CHECK(l.scale == 1 && l.top.visible && !l.bottom.visible && l.top.width == 400 && l.top.height == 240);
    l = ChooseLayout(200, 120);
    CHECK(l.scale == 2 && l.top.visible && !l.bottom.visible && l.top.width == 200 && l.top.dstX == 0);
    l = ChooseLayout(320, 240);
    CHECK(l.scale == 1 && !l.top.visible && l.bottom.visible && l.bottom.width == 320);
    l = ChooseLayout(160, 120);
    CHECK(l.scale == 2 && l.bottom.visible && !l.top.visible);
    l = ChooseLayout(400, 480);
    CHECK(l.top.visible && l.bottom.visible && l.top.height == 240 && l.bottom.srcY == 240
        && l.bottom.srcX == 40 && l.bottom.width == 320);
    l = ChooseLayout(200, 240);
    CHECK(l.scale == 2 && l.top.visible && l.bottom.visible && l.bottom.srcX == 20 && l.bottom.srcY == 120);
    l = ChooseLayout(640, 360); // some other video: cropped to the center
    CHECK(l.top.visible && l.top.width == 400 && l.top.srcX == 120 && l.top.srcY == 60);
    l = ChooseLayout(256, 192);
    CHECK(l.top.visible && l.top.dstX == 72 && l.top.dstY == 24);

    // Pixel placement: every source pixel gets a unique color
    const uint32_t w = 400, h = 480;
    std::vector<uint8_t> src(w * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t *p = &src[(y * w + x) * 3];
            p[0] = x & 0xFF; p[1] = y & 0xFF; p[2] = ((x >> 8) & 3) | (((y >> 8) & 3) << 2);
        }

    std::vector<uint8_t> top(400 * 240 * 3, 0xEE), bottom(320 * 240 * 3, 0xEE);
    FbTarget topFb = { top.data(), 400, FbFormat::Bgr8 };
    FbTarget bottomFb = { bottom.data(), 320, FbFormat::Bgr8 };
    l = ChooseLayout(w, h);
    Blit(src.data(), w, l.top, l.scale, topFb);
    Blit(src.data(), w, l.bottom, l.scale, bottomFb);

    for (uint32_t y = 0; y < 240; y += 7)
        for (uint32_t x = 0; x < 400; x += 3)
            CHECK(memcmp(FbPixel(top, x, y), &src[(y * w + x) * 3], 3) == 0);
    for (uint32_t y = 0; y < 240; y += 7)
        for (uint32_t x = 0; x < 320; x += 3)
        {
            // Bottom screen pixel (x, y) shows video pixel (x + 40, y + 240)
            const uint8_t *fb = &bottom[(x * 240 + (239 - y)) * 3];
            CHECK(memcmp(fb, &src[((y + 240) * w + x + 40) * 3], 3) == 0);
        }

    // 2x scaling, and nothing drawn outside the picture
    std::fill(top.begin(), top.end(), 0xEE);
    l = ChooseLayout(160, 100);
    Blit(src.data(), w, { true, 0, 0, 160, 100, l.top.dstX, l.top.dstY }, l.scale, topFb);
    CHECK(l.scale == 2 && l.top.dstX == 40 && l.top.dstY == 20);
    CHECK(memcmp(FbPixel(top, 40, 20), &src[0], 3) == 0);
    CHECK(memcmp(FbPixel(top, 41, 21), &src[0], 3) == 0);
    CHECK(memcmp(FbPixel(top, 42, 20), &src[3], 3) == 0);
    CHECK(memcmp(FbPixel(top, 40, 22), &src[w * 3], 3) == 0);
    CHECK(memcmp(FbPixel(top, 359, 219), &src[(99 * w + 159) * 3], 3) == 0);
    CHECK(FbPixel(top, 39, 20)[0] == 0xEE && FbPixel(top, 360, 20)[0] == 0xEE);
    CHECK(FbPixel(top, 40, 19)[0] == 0xEE && FbPixel(top, 40, 220)[0] == 0xEE);

    // RGB565 target (text console screen)
    std::vector<uint8_t> b565(320 * 240 * 2);
    FbTarget fb565 = { b565.data(), 320, FbFormat::Rgb565 };
    uint8_t white[3] = { 255, 255, 255 };
    Blit(white, 1, { true, 0, 0, 1, 1, 5, 6 }, 1, fb565);
    uint16_t c;
    memcpy(&c, &b565[(5 * 240 + (239 - 6)) * 2], 2);
    CHECK(c == 0xFFFF);

    printf("blit ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "info"))
        return Info(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "audio"))
        return Audio(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "frame0"))
        return Frame0(argv[2], argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "blit"))
        return Blit();
    return 2;
}
