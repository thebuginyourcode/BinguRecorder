#include "AviWriter.hpp"

#include <cstring>

namespace Bingu
{
    // File layout:
    //   RIFF <size> 'AVI '
    //     LIST <size> 'hdrl'
    //       avih
    //       LIST <size> 'strl'   video: strh + strf (BITMAPINFOHEADER)
    //       LIST <size> 'strl'   audio: strh + strf (WAVEFORMATEX), optional
    //     LIST <size> 'movi'
    //       00db / 00dc video chunks, 01wb audio chunks
    //     idx1

    // Update the header every N chunks so a recording interrupted by a crash
    // or power loss is still readable (players can rebuild the missing index).
    static constexpr uint32_t   PatchInterval = 60;

    static constexpr uint32_t   AVIF_HASINDEX = 0x10;
    static constexpr uint32_t   AVIF_ISINTERLEAVED = 0x100;
    static constexpr uint32_t   AVIIF_KEYFRAME = 0x10;
    static constexpr uint16_t   WAVE_FORMAT_IMA_ADPCM = 0x11;

    static constexpr uint32_t   MaxHeaderSize = 512;

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

    // Little helper to lay out the header
    struct HeaderBuilder
    {
        uint8_t    *base;
        uint32_t    pos;

        uint32_t    Tag(const char *tag)  { std::memcpy(base + pos, tag, 4); pos += 4; return pos - 4; }
        uint32_t    U32(uint32_t v)       { Put32(base + pos, v); pos += 4; return pos - 4; }
        uint32_t    U16(uint16_t v)       { Put16(base + pos, v); pos += 2; return pos - 2; }

        // Starts a LIST or chunk; returns the offset of its size field
        uint32_t    Begin(const char *tag) { Tag(tag); return U32(0); }
        void        End(uint32_t sizeOffset) { Put32(base + sizeOffset, pos - sizeOffset - 4); }
    };

    bool    AviWriter::Begin(AviSink *sink, const VideoFormat &video, const AudioFormat *audio)
    {
        const bool mjpeg = video.codec == Codec::Mjpeg;

        _sink = sink;
        _video = video;
        _hasAudio = audio != nullptr;
        if (audio)
            _audio = *audio;
        std::memcpy(_chunkIds[0], mjpeg ? "00dc" : "00db", 4);
        std::memcpy(_chunkIds[1], "01wb", 4);
        _position = 0;
        _frames = 0;
        _audioBlocks = 0;
        _chunksSincePatch = 0;
        _index.clear();
        _index.reserve(1024);

        // Raw: RGB555, width is always even so rows are 4-byte aligned
        const uint32_t imageSize = video.width * video.height * (mjpeg ? 3 : 2);
        const uint32_t bufferSize = (mjpeg ? video.maxFrameSize : imageSize) + ChunkHeaderSize;
        const uint32_t audioBytesPerSec = _hasAudio ?
            (uint32_t)((uint64_t)_audio.sampleRate * _audio.blockAlign / _audio.samplesPerBlock) : 0;

        uint8_t h[MaxHeaderSize];
        std::memset(h, 0, sizeof(h));
        HeaderBuilder b{h, 0};

        uint32_t riff = b.Begin("RIFF");
        b.Tag("AVI ");

        uint32_t hdrl = b.Begin("LIST");
        b.Tag("hdrl");

        // Main header
        b.Tag("avih");
        b.U32(56);
        b.U32(1000000 / video.fps);                    // dwMicroSecPerFrame
        b.U32(bufferSize * video.fps + audioBytesPerSec); // dwMaxBytesPerSec
        b.U32(0);                                       // dwPaddingGranularity
        b.U32(AVIF_HASINDEX | (_hasAudio ? AVIF_ISINTERLEAVED : 0));
        _totalFramesOffset = b.U32(0);                  // dwTotalFrames
        b.U32(0);                                       // dwInitialFrames
        b.U32(_hasAudio ? 2 : 1);                       // dwStreams
        b.U32(bufferSize);                              // dwSuggestedBufferSize
        b.U32(video.width);
        b.U32(video.height);
        b.pos += 16;                                    // dwReserved

        // Video stream
        uint32_t strl = b.Begin("LIST");
        b.Tag("strl");
        b.Tag("strh");
        b.U32(56);
        b.Tag("vids");
        b.Tag(mjpeg ? "MJPG" : "DIB ");
        b.U32(0);                                       // dwFlags
        b.U32(0);                                       // wPriority, wLanguage
        b.U32(0);                                       // dwInitialFrames
        b.U32(1);                                       // dwScale
        b.U32(video.fps);                               // dwRate
        b.U32(0);                                       // dwStart
        _videoLengthOffset = b.U32(0);                  // dwLength
        b.U32(bufferSize);                              // dwSuggestedBufferSize
        b.U32(0xFFFFFFFF);                              // dwQuality
        b.U32(0);                                       // dwSampleSize
        b.U16(0); b.U16(0);                             // rcFrame
        b.U16(video.width); b.U16(video.height);

        b.Tag("strf");
        b.U32(40);
        b.U32(40);                                      // biSize
        b.U32(video.width);
        b.U32(video.height);                            // positive: bottom-up rows
        b.U16(1);                                       // biPlanes
        if (mjpeg)
        {
            b.U16(24);                                  // biBitCount
            b.Tag("MJPG");                              // biCompression
        }
        else
        {
            b.U16(16);                                  // BI_RGB 16 bit == RGB555
            b.U32(0);
        }
        b.U32(imageSize);                               // biSizeImage
        b.pos += 16;                                    // resolution, palette
        b.End(strl);

        // Audio stream (same conventions as ffmpeg uses for ADPCM)
        if (_hasAudio)
        {
            strl = b.Begin("LIST");
            b.Tag("strl");
            b.Tag("strh");
            b.U32(56);
            b.Tag("auds");
            b.U32(0);                                   // fccHandler
            b.U32(0);                                   // dwFlags
            b.U32(0);                                   // wPriority, wLanguage
            b.U32(0);                                   // dwInitialFrames
            b.U32(_audio.samplesPerBlock);              // dwScale
            b.U32(_audio.sampleRate);                   // dwRate
            b.U32(0);                                   // dwStart
            _audioLengthOffset = b.U32(0);              // dwLength (blocks)
            b.U32(_audio.blockAlign * 8);               // dwSuggestedBufferSize
            b.U32(0xFFFFFFFF);                          // dwQuality
            b.U32(_audio.blockAlign);                   // dwSampleSize
            b.pos += 8;                                 // rcFrame

            b.Tag("strf");
            b.U32(20);
            b.U16(WAVE_FORMAT_IMA_ADPCM);
            b.U16(_audio.channels);
            b.U32(_audio.sampleRate);
            b.U32(audioBytesPerSec);
            b.U16(_audio.blockAlign);
            b.U16(4);                                   // wBitsPerSample
            b.U16(2);                                   // cbSize
            b.U16(_audio.samplesPerBlock);
            b.End(strl);
        }

        b.End(hdrl);

        _moviSizeOffset = b.Begin("LIST");
        _moviStart = b.Tag("movi");
        _headerSize = b.pos;
        (void)riff; // RIFF size is filled in by PatchSizes()

        if (!Append(h, _headerSize))
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

    bool    AviWriter::AddChunk(uint8_t *chunk, uint32_t stream, uint32_t size)
    {
        std::memcpy(chunk, _chunkIds[stream], 4);
        Put32(chunk + 4, size);

        // RIFF chunks are padded to an even size
        uint32_t padded = size + (size & 1);
        if (size & 1)
            chunk[ChunkHeaderSize + size] = 0;

        uint32_t offset = _position - _moviStart;

        if (!Append(chunk, padded + ChunkHeaderSize))
            return false;

        _index.push_back({offset, size, stream});

        if (++_chunksSincePatch >= PatchInterval)
            return PatchSizes();
        return true;
    }

    bool    AviWriter::WriteFrame(uint8_t *chunk, uint32_t size)
    {
        ++_frames;
        return AddChunk(chunk, 0, size);
    }

    bool    AviWriter::WriteAudio(uint8_t *chunk, uint32_t blocks)
    {
        if (!_hasAudio || !blocks)
            return true;

        _audioBlocks += blocks;
        return AddChunk(chunk, 1, blocks * _audio.blockAlign);
    }

    bool    AviWriter::WriteDropped(uint32_t count)
    {
        uint8_t empty[ChunkHeaderSize * 16];

        for (uint32_t i = 0; i < 16; ++i)
        {
            std::memcpy(empty + i * ChunkHeaderSize, _chunkIds[0], 4);
            Put32(empty + i * ChunkHeaderSize + 4, 0);
        }

        while (count)
        {
            uint32_t n = count > 16 ? 16 : count;
            uint32_t offset = _position - _moviStart;

            if (!Append(empty, n * ChunkHeaderSize))
                return false;

            for (uint32_t i = 0; i < n; ++i)
                _index.push_back({offset + i * ChunkHeaderSize, 0, 0});

            count -= n;
            _frames += n;
            _chunksSincePatch += n;
        }

        if (_chunksSincePatch >= PatchInterval)
            return PatchSizes();
        return true;
    }

    uint32_t    AviWriter::ProjectedSize(uint32_t extraFrames, uint32_t frameSize) const
    {
        uint32_t chunks = (uint32_t)_index.size() + extraFrames;
        return _position + extraFrames * (frameSize + 1 + ChunkHeaderSize) + 8 + chunks * 16;
    }

    bool    AviWriter::Patch32(uint32_t offset, uint32_t value)
    {
        uint8_t v[4];

        Put32(v, value);
        return _sink->Seek(offset) && _sink->Write(v, 4);
    }

    bool    AviWriter::PatchSizes(void)
    {
        const uint32_t end = _position;

        _chunksSincePatch = 0;

        // While recording, the movi list runs to the end of the file. Once
        // idx1 is appended Finish() fixes this up to stop before it.
        if (!Patch32(4, end - 8)
            || !Patch32(_totalFramesOffset, _frames)
            || !Patch32(_videoLengthOffset, _frames)
            || (_hasAudio && !Patch32(_audioLengthOffset, _audioBlocks))
            || !Patch32(_moviSizeOffset, end - _moviSizeOffset - 4))
            return false;

        return _sink->Seek(end);
    }

    bool    AviWriter::Finish(void)
    {
        if (!_sink)
            return false;

        const uint32_t moviEnd = _position;

        // Write the index in blocks to keep the number of SD writes low
        uint8_t block[16 * 64];
        uint8_t header[8];

        std::memcpy(header, "idx1", 4);
        Put32(header + 4, (uint32_t)_index.size() * 16);
        if (!Append(header, 8))
            return false;

        uint32_t used = 0;
        for (const IndexEntry &e : _index)
        {
            uint8_t *p = block + used;

            std::memcpy(p, _chunkIds[e.stream], 4);
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

        if (!PatchSizes() || !Patch32(_moviSizeOffset, moviEnd - _moviSizeOffset - 4))
            return false;

        _sink = nullptr;
        _index.clear();
        _index.shrink_to_fit();
        return true;
    }
}
