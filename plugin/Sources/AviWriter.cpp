#include "AviWriter.hpp"

#include <cstring>

namespace Bingu
{
    // Fixed header layout (offsets into the file):
    //   0   RIFF <size> 'AVI '
    //   12  LIST <size> 'hdrl'
    //   24    avih <56>               data @ 32
    //   88    LIST <size> 'strl'
    //   100     strh <56>             data @ 108
    //   164     strf <40>             data @ 172
    //   212 LIST <size> 'movi'
    //   224   00db chunks...
    //   ...   idx1
    static constexpr uint32_t   HeaderSize = 224;
    static constexpr uint32_t   RiffSizeOffset = 4;
    static constexpr uint32_t   TotalFramesOffset = 32 + 16;
    static constexpr uint32_t   StreamLengthOffset = 108 + 32;
    static constexpr uint32_t   MoviSizeOffset = 216;
    static constexpr uint32_t   MoviFourccOffset = 220;

    // Update the header every N frames so a recording interrupted by a crash
    // or power loss is still readable (players can rebuild the missing index).
    static constexpr uint32_t   PatchInterval = 60;

    static constexpr uint32_t   AVIF_HASINDEX = 0x10;
    static constexpr uint32_t   AVIIF_KEYFRAME = 0x10;

    static void     Put32(uint8_t *p, uint32_t v)
    {
        p[0] = v & 0xFF;
        p[1] = (v >> 8) & 0xFF;
        p[2] = (v >> 16) & 0xFF;
        p[3] = (v >> 24) & 0xFF;
    }

    static void     Put16(uint8_t *p, uint16_t v)
    {
        p[0] = v & 0xFF;
        p[1] = v >> 8;
    }

    static void     PutTag(uint8_t *p, const char *tag)
    {
        std::memcpy(p, tag, 4);
    }

    bool    AviWriter::Begin(AviSink *sink, uint32_t width, uint32_t height, uint32_t fps)
    {
        _sink = sink;
        _width = width;
        _height = height;
        _fps = fps;
        _frameSize = width * height * 2; // RGB555, width is always even so rows are 4-byte aligned
        _position = 0;
        _framesSincePatch = 0;
        _index.clear();
        _index.reserve(1024);

        uint8_t h[HeaderSize];
        std::memset(h, 0, sizeof(h));

        PutTag(h + 0, "RIFF");
        PutTag(h + 8, "AVI ");

        PutTag(h + 12, "LIST");
        Put32(h + 16, 212 - 20);
        PutTag(h + 20, "hdrl");

        PutTag(h + 24, "avih");
        Put32(h + 28, 56);
        uint8_t *avih = h + 32;
        Put32(avih + 0, 1000000 / fps);     // dwMicroSecPerFrame
        Put32(avih + 4, _frameSize * fps);  // dwMaxBytesPerSec
        Put32(avih + 12, AVIF_HASINDEX);    // dwFlags
        Put32(avih + 24, 1);                // dwStreams
        Put32(avih + 28, _frameSize + ChunkHeaderSize); // dwSuggestedBufferSize
        Put32(avih + 32, width);
        Put32(avih + 36, height);

        PutTag(h + 88, "LIST");
        Put32(h + 92, 212 - 96);
        PutTag(h + 96, "strl");

        PutTag(h + 100, "strh");
        Put32(h + 104, 56);
        uint8_t *strh = h + 108;
        PutTag(strh + 0, "vids");
        PutTag(strh + 4, "DIB ");
        Put32(strh + 20, 1);                // dwScale
        Put32(strh + 24, fps);              // dwRate
        Put32(strh + 36, _frameSize + ChunkHeaderSize); // dwSuggestedBufferSize
        Put32(strh + 40, 0xFFFFFFFF);       // dwQuality
        Put16(strh + 52, (uint16_t)width);  // rcFrame.right
        Put16(strh + 54, (uint16_t)height); // rcFrame.bottom

        PutTag(h + 164, "strf");
        Put32(h + 168, 40);
        uint8_t *bih = h + 172;
        Put32(bih + 0, 40);                 // biSize
        Put32(bih + 4, width);
        Put32(bih + 8, height);             // positive: bottom-up rows
        Put16(bih + 12, 1);                 // biPlanes
        Put16(bih + 14, 16);                // biBitCount (BI_RGB 16 bit == RGB555)
        Put32(bih + 20, _frameSize);        // biSizeImage

        PutTag(h + 212, "LIST");
        PutTag(h + 220, "movi");

        if (!Append(h, sizeof(h)))
            return false;
        return PatchSizes();
    }

    bool    AviWriter::Append(const void *data, uint32_t size)
    {
        if (!_sink->Write(data, size))
            return false;
        _position += size;
        return true;
    }

    bool    AviWriter::WriteFrame(uint8_t *chunk)
    {
        PutTag(chunk, "00db");
        Put32(chunk + 4, _frameSize);

        uint32_t offset = _position - MoviFourccOffset;

        if (!Append(chunk, _frameSize + ChunkHeaderSize))
            return false;

        _index.push_back({offset, _frameSize});

        if (++_framesSincePatch >= PatchInterval)
            return PatchSizes();
        return true;
    }

    bool    AviWriter::WriteDropped(uint32_t count)
    {
        uint8_t empty[ChunkHeaderSize * 16];

        for (uint32_t i = 0; i < 16; ++i)
        {
            PutTag(empty + i * ChunkHeaderSize, "00db");
            Put32(empty + i * ChunkHeaderSize + 4, 0);
        }

        while (count)
        {
            uint32_t n = count > 16 ? 16 : count;
            uint32_t offset = _position - MoviFourccOffset;

            if (!Append(empty, n * ChunkHeaderSize))
                return false;

            for (uint32_t i = 0; i < n; ++i)
                _index.push_back({offset + i * ChunkHeaderSize, 0});

            count -= n;
            _framesSincePatch += n;
        }

        if (_framesSincePatch >= PatchInterval)
            return PatchSizes();
        return true;
    }

    uint32_t    AviWriter::ProjectedSize(uint32_t extraFrames) const
    {
        uint32_t frames = FrameCount() + extraFrames;
        return _position + extraFrames * (_frameSize + ChunkHeaderSize) + 8 + frames * 16;
    }

    bool    AviWriter::PatchSizes(void)
    {
        uint8_t v[4];
        uint32_t frames = FrameCount();
        uint32_t end = _position;

        _framesSincePatch = 0;

        Put32(v, end - 8);
        if (!_sink->Seek(RiffSizeOffset) || !_sink->Write(v, 4))
            return false;

        Put32(v, frames);
        if (!_sink->Seek(TotalFramesOffset) || !_sink->Write(v, 4))
            return false;
        if (!_sink->Seek(StreamLengthOffset) || !_sink->Write(v, 4))
            return false;

        // While recording the movi list runs to the end of the file. Once
        // idx1 is appended Finish() fixes this up to stop before it.
        Put32(v, (end > HeaderSize ? end : HeaderSize) - MoviFourccOffset);
        if (!_sink->Seek(MoviSizeOffset) || !_sink->Write(v, 4))
            return false;

        return _sink->Seek(end);
    }

    bool    AviWriter::Finish(void)
    {
        if (!_sink)
            return false;

        uint32_t moviEnd = _position;

        // Write the index in blocks to keep the number of SD writes low
        uint8_t block[16 * 64];
        uint8_t header[8];

        PutTag(header, "idx1");
        Put32(header + 4, FrameCount() * 16);
        if (!Append(header, 8))
            return false;

        uint32_t used = 0;
        for (const IndexEntry &e : _index)
        {
            uint8_t *p = block + used;

            PutTag(p, "00db");
            Put32(p + 4, e.size ? AVIIF_KEYFRAME : 0);
            Put32(p + 8, e.offset);
            Put32(p + 12, e.size);
            used += 16;

            if (used == sizeof(block))
            {
                if (!Append(block, used))
                    return false;
                used = 0;
            }
        }

        if (used && !Append(block, used))
            return false;

        if (!PatchSizes())
            return false;

        uint8_t v[4];
        Put32(v, moviEnd - MoviFourccOffset);
        if (!_sink->Seek(MoviSizeOffset) || !_sink->Write(v, 4))
            return false;

        _sink = nullptr;
        _index.clear();
        _index.shrink_to_fit();
        return true;
    }
}
