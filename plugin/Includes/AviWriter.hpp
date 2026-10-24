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

    enum class Codec : uint8_t
    {
        Raw,    ///< 16-bit RGB555 bitmaps (bottom-up rows), lossless but huge
        Mjpeg,  ///< One JPEG per frame
    };

    struct VideoFormat
    {
        uint32_t    width;
        uint32_t    height;
        uint32_t    fps;
        Codec       codec;
        uint32_t    maxFrameSize;   ///< Largest frame that will be written (hint for players)
    };

    // IMA ADPCM audio, as in WAV files
    struct AudioFormat
    {
        uint32_t    sampleRate;
        uint16_t    channels;
        uint16_t    blockAlign;         ///< Bytes per ADPCM block (all channels)
        uint16_t    samplesPerBlock;    ///< Per channel
    };

    // Minimal AVI 1.0 writer: one video stream and optionally one audio stream.
    //
    // Every captured "screenshot" becomes one video chunk. Frames that were
    // skipped because the 3DS couldn't keep up are written as empty chunks,
    // which players treat as "repeat the previous frame", so the video always
    // plays back in real time.
    class AviWriter
    {
    public:
        static constexpr uint32_t ChunkHeaderSize = 8;

        AviWriter() = default;

        // `audio` may be null for a video-only file
        bool        Begin(AviSink *sink, const VideoFormat &video, const AudioFormat *audio = nullptr);

        // `chunk` must point to ChunkHeaderSize free bytes, then `size` bytes
        // of frame data, then one spare byte (for padding odd sizes). The
        // header is filled in here so the whole frame goes to the SD card in
        // a single write. For Raw, `size` must be RawFrameSize().
        bool        WriteFrame(uint8_t *chunk, uint32_t size);

        // Appends `count` empty (duplicate) frames.
        bool        WriteDropped(uint32_t count);

        // Same buffer layout as WriteFrame, holding `blocks` ADPCM blocks.
        bool        WriteAudio(uint8_t *chunk, uint32_t blocks);

        // Writes the index and fixes up all sizes. The sink can be closed after.
        bool        Finish(void);

        uint32_t    RawFrameSize(void) const { return _video.width * _video.height * 2; }
        uint32_t    FrameCount(void) const { return _frames; }
        uint32_t    FileSize(void) const { return _position; }

        // Upper bound on the file size once the index is appended, after
        // writing `extraFrames` more frames of at most `frameSize` bytes.
        uint32_t    ProjectedSize(uint32_t extraFrames, uint32_t frameSize) const;

    private:
        struct IndexEntry
        {
            uint32_t    offset;
            uint32_t    size;
            uint32_t    stream;
        };

        bool        Append(const void *data, uint32_t size);
        bool        AddChunk(uint8_t *chunk, uint32_t stream, uint32_t size);
        bool        PatchSizes(void);
        bool        Patch32(uint32_t offset, uint32_t value);

        AviSink *               _sink{nullptr};
        VideoFormat             _video{};
        AudioFormat             _audio{};
        bool                    _hasAudio{false};
        char                    _chunkIds[2][4];
        uint32_t                _position{0};
        uint32_t                _frames{0};
        uint32_t                _audioBlocks{0};
        uint32_t                _chunksSincePatch{0};

        // Header fields updated as the file grows
        uint32_t                _totalFramesOffset{0};
        uint32_t                _videoLengthOffset{0};
        uint32_t                _audioLengthOffset{0};
        uint32_t                _moviSizeOffset{0};
        uint32_t                _moviStart{0};  ///< File offset of the 'movi' fourcc
        uint32_t                _headerSize{0};

        std::vector<IndexEntry> _index;
    };
}

#endif
