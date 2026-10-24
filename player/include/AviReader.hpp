#ifndef BINGU_AVIREADER_HPP
#define BINGU_AVIREADER_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Bingu
{
    // Reads the AVI files BinguRecorder writes (and other simple AVI 1.0
    // files): one video stream (MJPEG or 16-bit RGB) and optionally one IMA
    // ADPCM audio stream. Uses the idx1 index when there is one, otherwise
    // scans the file, so recordings that were cut off still play.
    class AviReader
    {
    public:
        enum class VideoCodec
        {
            Mjpeg,
            Rgb555,     ///< BI_RGB 16 bit, bottom-up rows
        };

        struct Chunk
        {
            uint32_t    offset;     ///< File offset of the data
            uint32_t    size;       ///< 0 for a "repeat the previous frame" video chunk
        };

        struct AudioChunk : Chunk
        {
            uint32_t    firstSample;    ///< Per channel, from the start of the stream
        };

        ~AviReader() { Close(); }

        // Returns an empty string on success, otherwise what went wrong
        std::string Open(const std::string &path);
        void        Close(void);

        uint32_t    Width(void) const { return _width; }
        uint32_t    Height(void) const { return _height; }
        VideoCodec  Codec(void) const { return _codec; }
        double      Fps(void) const { return _fps; }
        uint32_t    FrameCount(void) const { return (uint32_t)_frames.size(); }
        uint32_t    MaxVideoChunk(void) const { return _maxVideoChunk; }
        const Chunk &Frame(uint32_t i) const { return _frames[i]; }

        // Index of the last frame <= i that has data (-1 if none)
        int32_t     LastRealFrame(uint32_t i) const;

        bool        HasAudio(void) const { return _hasAudio; }
        uint32_t    SampleRate(void) const { return _sampleRate; }
        uint32_t    Channels(void) const { return _channels; }
        uint32_t    BlockAlign(void) const { return _blockAlign; }
        uint32_t    SamplesPerBlock(void) const { return _samplesPerBlock; }
        uint32_t    AudioChunkCount(void) const { return (uint32_t)_audio.size(); }
        uint32_t    MaxAudioChunk(void) const { return _maxAudioChunk; }
        const AudioChunk &Audio(uint32_t i) const { return _audio[i]; }
        // Last audio chunk starting at or before `sample`
        uint32_t    AudioChunkAt(uint32_t sample) const;

        double      Duration(void) const { return _frames.size() / _fps; }

        bool        Read(const Chunk &chunk, void *out) const;

    private:
        bool        ParseHeaders(uint32_t start, uint32_t end);
        bool        ReadIndex(uint32_t start, uint32_t size);
        void        ScanMovi(uint32_t start, uint32_t end);
        void        AddChunk(uint32_t fourcc, uint32_t offset, uint32_t size);

        FILE *                  _file{nullptr};
        uint32_t                _fileSize{0};
        uint32_t                _moviStart{0};  ///< Offset of the 'movi' fourcc
        uint32_t                _moviEnd{0};

        uint32_t                _width{0};
        uint32_t                _height{0};
        VideoCodec              _codec{VideoCodec::Mjpeg};
        double                  _fps{0};
        int                     _videoStream{-1};
        int                     _audioStream{-1};

        bool                    _hasAudio{false};
        uint32_t                _sampleRate{0};
        uint32_t                _channels{0};
        uint32_t                _blockAlign{0};
        uint32_t                _samplesPerBlock{0};

        std::vector<Chunk>      _frames;
        std::vector<AudioChunk> _audio;
        uint32_t                _audioSamples{0};
        uint32_t                _maxVideoChunk{0};
        uint32_t                _maxAudioChunk{0};
    };
}

#endif
