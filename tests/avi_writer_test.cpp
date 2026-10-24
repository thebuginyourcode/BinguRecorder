// Host-side test for the AVI writer: builds a short clip with real and dropped
// frames, then (via run_tests.sh) checks it with ffprobe/ffmpeg.
#include "AviWriter.hpp"

#include <cstdio>
#include <cstdlib>
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

int main(int argc, char **argv)
{
    if (argc < 3)
        return 1;

    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 1;

    // finish=0 simulates a recording that was cut off (no idx1)
    bool finish = atoi(argv[2]) != 0;
    FileSink sink(f);
    Bingu::AviWriter avi;
    const uint32_t w = 400, h = 240;

    if (!avi.Begin(&sink, w, h, 10))
        return 2;

    std::vector<uint8_t> chunk(Bingu::AviWriter::ChunkHeaderSize + avi.FrameSize());
    uint16_t *px = (uint16_t *)(chunk.data() + Bingu::AviWriter::ChunkHeaderSize);

    for (uint32_t frame = 0; frame < 100; ++frame)
    {
        // Pure red (RGB555) on odd frames, pure blue on even ones; bottom row green
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                px[y * w + x] = y == 0 ? (0x1F << 5) : (frame & 1) ? (0x1F << 10) : 0x1F;

        if (!avi.WriteFrame(chunk.data()))
            return 3;
        if (frame % 10 == 9 && !avi.WriteDropped(3))
            return 4;
    }

    if (finish && !avi.Finish())
        return 5;
    fclose(f);
    printf("frames=%u\n", avi.FrameCount());
    return 0;
}
