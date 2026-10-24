// Host-side test for the AVI writer and JPEG encoder: builds a short clip with
// real and dropped frames, then (via run_tests.sh) checks it with ffmpeg.
//
// usage: avitest <out.avi> <finish 0|1> <raw|mjpeg> <width> <height> <none|stereo|mono>
//                [ref.rgb] [ref.pcm]
#include "AviWriter.hpp"
#include "ImaAdpcm.hpp"
#include "JpegEncoder.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

class FileSink : public Bingu::AviSink
{
public:
    explicit FileSink(FILE *f) : _f(f) {}
    bool Write(const void *data, uint32_t size) override { return fwrite(data, 1, size, _f) == size; }
    bool Seek(uint32_t offset) override { return fseek(_f, offset, SEEK_SET) == 0; }
private:
    FILE *_f;
};

static uint16_t Rgb555(int r, int g, int b)
{
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

// Frame 0 has a smooth "game-like" picture with a solid blue top-left corner
// and a solid green bottom-left corner (to check orientation). Later frames
// alternate the corners between red and blue.
static void DrawFrame(uint16_t *px, uint32_t w, uint32_t h, uint32_t frame)
{
    for (uint32_t y = 0; y < h; ++y)      // y = 0 is the bottom row
    {
        uint32_t ty = h - 1 - y;          // top-down coordinate
        for (uint32_t x = 0; x < w; ++x)
        {
            int r = x * 255 / w;
            int g = ty * 255 / h;
            int b = (int)(128 + 100 * sin(x * 0.1 + frame) * cos(ty * 0.07));
            uint16_t c = Rgb555(r, g, b);

            if (x < 32 && ty < 32)
                c = frame & 1 ? Rgb555(255, 0, 0) : Rgb555(0, 0, 255);
            else if (x < 32 && ty >= h - 32)
                c = Rgb555(0, 255, 0);
            px[y * w + x] = c;
        }
    }
}

// Test tone: 440 Hz left, 660 Hz right (the same tone on both for mono)
static int16_t Tone(uint64_t n, uint32_t rate, uint32_t channel)
{
    double f = channel ? 660.0 : 440.0;
    return (int16_t)(12000 * sin(2 * M_PI * f * n / rate));
}

int main(int argc, char **argv)
{
    if (argc < 7)
        return 1;

    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 1;

    bool finish = atoi(argv[2]) != 0;   // 0 simulates a recording that was cut off (no idx1)
    bool mjpeg = strcmp(argv[3], "mjpeg") == 0;
    uint32_t w = atoi(argv[4]), h = atoi(argv[5]);
    bool audio = strcmp(argv[6], "none") != 0;
    uint32_t channels = strcmp(argv[6], "mono") == 0 ? 1 : 2;
    uint32_t rate = channels == 1 ? 16364 : 32728;

    FileSink sink(f);
    Bingu::AviWriter avi;
    Bingu::JpegEncoder jpeg(85);
    const uint32_t rawSize = w * h * 2;
    const uint32_t outSize = Bingu::AviWriter::ChunkHeaderSize + w * h + 1;

    Bingu::ImaAdpcmEncoder adpcm(channels);
    Bingu::AudioFormat af = { rate, (uint16_t)channels, (uint16_t)adpcm.BlockSize(),
                              (uint16_t)Bingu::ImaAdpcmEncoder::SamplesPerBlock };
    Bingu::VideoFormat vf = { w, h, 10, mjpeg ? Bingu::Codec::Mjpeg : Bingu::Codec::Raw, outSize };

    if (!avi.Begin(&sink, vf, audio ? &af : nullptr))
        return 2;

    const uint32_t spb = Bingu::ImaAdpcmEncoder::SamplesPerBlock;
    std::vector<int16_t> pcm(spb * channels);
    std::vector<uint8_t> audioChunk(Bingu::AviWriter::ChunkHeaderSize + adpcm.BlockSize() + 1);
    uint64_t samplesWritten = 0;
    FILE *refPcm = audio && argc > 8 ? fopen(argv[8], "wb") : nullptr;

    std::vector<uint8_t> chunk(Bingu::AviWriter::ChunkHeaderSize + rawSize + 1);
    std::vector<uint8_t> out(outSize);
    uint16_t *px = (uint16_t *)(chunk.data() + Bingu::AviWriter::ChunkHeaderSize);
    uint64_t bytes = 0;

    for (uint32_t frame = 0; frame < 100; ++frame)
    {
        DrawFrame(px, w, h, frame);

        if (frame == 0 && argc > 7)
        {
            // Reference picture for the quality check (top-down RGB24)
            FILE *ref = fopen(argv[7], "wb");
            for (uint32_t ty = 0; ty < h; ++ty)
                for (uint32_t x = 0; x < w; ++x)
                {
                    uint16_t p = px[(h - 1 - ty) * w + x];
                    uint8_t c[3] = { uint8_t(((p >> 10) & 31) * 255 / 31), uint8_t(((p >> 5) & 31) * 255 / 31), uint8_t((p & 31) * 255 / 31) };
                    fwrite(c, 1, 3, ref);
                }
            fclose(ref);
        }

        if (mjpeg)
        {
            uint32_t n = jpeg.Encode(px, w, h, out.data() + Bingu::AviWriter::ChunkHeaderSize,
                                     outSize - Bingu::AviWriter::ChunkHeaderSize - 1);
            if (!n || !avi.WriteFrame(out.data(), n))
                return 3;
            bytes += n;
        }
        else
        {
            if (!avi.WriteFrame(chunk.data(), rawSize))
                return 3;
            bytes += rawSize;
        }

        if (frame % 10 == 9 && !avi.WriteDropped(3))
            return 4;

        // Audio up to the end of the frames written so far, interleaved
        // the way the recorder does it
        uint64_t due = (uint64_t)avi.FrameCount() * rate / 10;
        while (audio && samplesWritten + spb <= due)
        {
            for (uint32_t i = 0; i < spb; ++i)
                for (uint32_t c = 0; c < channels; ++c)
                    pcm[i * channels + c] = Tone(samplesWritten + i, rate, c);
            if (refPcm)
                fwrite(pcm.data(), 2, pcm.size(), refPcm);

            adpcm.EncodeBlock(pcm.data(), audioChunk.data() + Bingu::AviWriter::ChunkHeaderSize);
            if (!avi.WriteAudio(audioChunk.data(), 1))
                return 6;
            samplesWritten += spb;
        }
    }
    if (refPcm)
        fclose(refPcm);

    printf("frames=%u avg_frame_bytes=%llu\n", avi.FrameCount(), (unsigned long long)(bytes / 100));
    if (finish && !avi.Finish())
        return 5;
    fclose(f);
    return 0;
}
