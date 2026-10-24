// Host-side test for the AVI writer and JPEG encoder: builds a short clip with
// real and dropped frames, then (via run_tests.sh) checks it with ffmpeg.
//
// usage: avitest <out.avi> <finish 0|1> <raw|mjpeg> <width> <height> [ref.rgb]
#include "AviWriter.hpp"
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

int main(int argc, char **argv)
{
    if (argc < 6)
        return 1;

    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 1;

    bool finish = atoi(argv[2]) != 0;   // 0 simulates a recording that was cut off (no idx1)
    bool mjpeg = strcmp(argv[3], "mjpeg") == 0;
    uint32_t w = atoi(argv[4]), h = atoi(argv[5]);

    FileSink sink(f);
    Bingu::AviWriter avi;
    Bingu::JpegEncoder jpeg(85);
    const uint32_t rawSize = w * h * 2;
    const uint32_t outSize = Bingu::AviWriter::ChunkHeaderSize + w * h + 1;

    if (!avi.Begin(&sink, w, h, 10, mjpeg ? Bingu::Codec::Mjpeg : Bingu::Codec::Raw, outSize))
        return 2;

    std::vector<uint8_t> chunk(Bingu::AviWriter::ChunkHeaderSize + rawSize + 1);
    std::vector<uint8_t> out(outSize);
    uint16_t *px = (uint16_t *)(chunk.data() + Bingu::AviWriter::ChunkHeaderSize);
    uint64_t bytes = 0;

    for (uint32_t frame = 0; frame < 100; ++frame)
    {
        DrawFrame(px, w, h, frame);

        if (frame == 0 && argc > 6)
        {
            // Reference picture for the quality check (top-down RGB24)
            FILE *ref = fopen(argv[6], "wb");
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
    }

    printf("frames=%u avg_frame_bytes=%llu\n", avi.FrameCount(), (unsigned long long)(bytes / 100));
    if (finish && !avi.Finish())
        return 5;
    fclose(f);
    return 0;
}
