#ifndef BINGU_AVIWRITER_HPP
#define BINGU_AVIWRITER_HPP

#include <cstdint>
#include <vector>

namespace Bingu
{
    // Where the AVI bytes go. On the 3DS this wraps a CTRPF File, on a PC
    // (tests) it wraps a FILE*.
    class AviSink
    {
    public:
        virtual ~AviSink() {}
        virtual bool Write(const void *data, uint32_t size) = 0;
        virtual bool Seek(uint32_t absoluteOffset) = 0;
    };

    // Minimal uncompressed AVI 1.0 writer.
    //
    // Every captured "screenshot" becomes one '00db' chunk of 16-bit RGB555
    // pixels (bottom-up rows, i.e. a plain BMP pixel array). Frames that were
    // skipped because the 3DS couldn't keep up are written as empty '00db'
    // chunks, which players treat as "repeat the previous frame", so the video
    // always plays back in real time.
    class AviWriter
    {
    public:
        static constexpr uint32_t ChunkHeaderSize = 8;

        AviWriter() = default;

        bool        Begin(AviSink *sink, uint32_t width, uint32_t height, uint32_t fps);

        // `chunk` must point to ChunkHeaderSize free bytes followed by
        // FrameSize() bytes of pixels. The header is filled in here so the
        // whole frame goes to the SD card in a single write.
        bool        WriteFrame(uint8_t *chunk);

        // Appends `count` empty (duplicate) frames.
        bool        WriteDropped(uint32_t count);

        // Writes the index and fixes up all sizes. The sink can be closed after.
        bool        Finish(void);

        uint32_t    FrameSize(void) const { return _frameSize; }
        uint32_t    FrameCount(void) const { return (uint32_t)_index.size(); }
        uint32_t    FileSize(void) const { return _position; }

        // Upper bound on the file size once the index is appended.
        uint32_t    ProjectedSize(uint32_t extraFrames) const;

    private:
        struct IndexEntry
        {
            uint32_t    offset;
            uint32_t    size;
        };

        bool        Append(const void *data, uint32_t size);
        bool        PatchSizes(void);

        AviSink *               _sink{nullptr};
        uint32_t                _width{0};
        uint32_t                _height{0};
        uint32_t                _fps{0};
        uint32_t                _frameSize{0};
        uint32_t                _position{0};
        uint32_t                _framesSincePatch{0};
        std::vector<IndexEntry> _index;
    };
}

#endif
