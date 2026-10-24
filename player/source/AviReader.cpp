#include "AviReader.hpp"

#include <algorithm>
#include <cstring>

namespace Bingu
{
    static uint32_t     Get32(const uint8_t *p)
    {
        return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    static uint16_t     Get16(const uint8_t *p)
    {
        return p[0] | (p[1] << 8);
    }

    static constexpr uint32_t   Tag(const char (&t)[5])
    {
        return (uint8_t)t[0] | ((uint8_t)t[1] << 8) | ((uint8_t)t[2] << 16) | ((uint32_t)(uint8_t)t[3] << 24);
    }

    // Stream number of a '##xx' chunk id, -1 if it isn't one
    static int      StreamOf(uint32_t fourcc)
    {
        int a = (fourcc & 0xFF) - '0';
        int b = ((fourcc >> 8) & 0xFF) - '0';

        if (a < 0 || a > 9 || b < 0 || b > 9)
            return -1;
        return a * 10 + b;
    }

    std::string     AviReader::Open(const std::string &path)
    {
        Close();

        _file = fopen(path.c_str(), "rb");
        if (!_file)
            return "Could not open the file";

        fseek(_file, 0, SEEK_END);
        _fileSize = (uint32_t)ftell(_file);
        fseek(_file, 0, SEEK_SET);

        uint8_t h[12];
        if (fread(h, 1, 12, _file) != 12 || Get32(h) != Tag("RIFF") || Get32(h + 8) != Tag("AVI "))
            return "Not an AVI file";

        // Walk the top level chunks: hdrl, movi, idx1
        uint32_t pos = 12;
        bool indexed = false;

        while (pos + 12 <= _fileSize)
        {
            uint8_t c[12];

            fseek(_file, pos, SEEK_SET);
            if (fread(c, 1, 12, _file) != 12)
                break;

            uint32_t id = Get32(c);
            uint32_t size = Get32(c + 4);
            uint32_t end = pos + 8 + size;

            // A recording that was cut off: the last sizes may be out of date
            if (size > _fileSize - pos - 8)
                end = _fileSize;

            if (id == Tag("LIST") && Get32(c + 8) == Tag("hdrl"))
            {
                if (!ParseHeaders(pos + 12, end))
                    return "Unsupported video format";
            }
            else if (id == Tag("LIST") && Get32(c + 8) == Tag("movi"))
            {
                _moviStart = pos + 8;
                _moviEnd = end;
            }
            else if (id == Tag("idx1") && _moviStart)
                indexed = ReadIndex(pos + 8, end - pos - 8);

            pos = end + (end & 1);
        }

        if (_videoStream < 0)
            return "No video in this file";
        if (!_moviStart)
            return "The file has no video data";

        if (!indexed)
        {
            // No index (recording was interrupted): find the chunks ourselves.
            // The data may run past the movi size recorded in the header.
            _frames.clear();
            _audio.clear();
            _audioSamples = 0;
            ScanMovi(_moviStart + 4, _fileSize);
        }

        if (_frames.empty())
            return "The video is empty";
        return "";
    }

    void    AviReader::Close(void)
    {
        if (_file)
            fclose(_file);
        _file = nullptr;
        _moviStart = _moviEnd = 0;
        _videoStream = _audioStream = -1;
        _hasAudio = false;
        _frames.clear();
        _audio.clear();
        _audioSamples = 0;
        _maxVideoChunk = _maxAudioChunk = 0;
    }

    bool    AviReader::ParseHeaders(uint32_t start, uint32_t end)
    {
        uint32_t pos = start;
        int stream = 0;

        while (pos + 8 <= end)
        {
            uint8_t c[12];

            fseek(_file, pos, SEEK_SET);
            if (fread(c, 1, 12, _file) < 8)
                return false;

            uint32_t id = Get32(c);
            uint32_t size = Get32(c + 4);

            if (id == Tag("LIST") && Get32(c + 8) == Tag("strl"))
            {
                // strh + strf of one stream
                uint32_t sp = pos + 12;
                uint32_t send = std::min(pos + 8 + size, end);
                uint32_t type = 0, scale = 1, rate = 0;

                while (sp + 8 <= send)
                {
                    uint8_t s[8];
                    fseek(_file, sp, SEEK_SET);
                    if (fread(s, 1, 8, _file) != 8)
                        return false;

                    uint32_t sid = Get32(s);
                    uint32_t ssize = Get32(s + 4);
                    uint8_t data[64] = {0};

                    if (fread(data, 1, std::min<uint32_t>(ssize, sizeof(data)), _file) == 0)
                        return false;

                    if (sid == Tag("strh"))
                    {
                        type = Get32(data);
                        scale = Get32(data + 20);
                        rate = Get32(data + 24);
                    }
                    else if (sid == Tag("strf") && type == Tag("vids") && _videoStream < 0)
                    {
                        uint32_t compression = Get32(data + 16);
                        uint16_t bits = Get16(data + 14);
                        int32_t height = (int32_t)Get32(data + 8);

                        _width = Get32(data + 4);
                        _height = height < 0 ? -height : height;

                        if (compression == Tag("MJPG"))
                            _codec = VideoCodec::Mjpeg;
                        else if (compression == 0 && bits == 16 && height > 0)
                            _codec = VideoCodec::Rgb555;
                        else
                            return false;

                        if (!_width || !_height || _width > 1024 || _height > 1024 || !scale || !rate)
                            return false;

                        _fps = (double)rate / scale;
                        _videoStream = stream;
                    }
                    else if (sid == Tag("strf") && type == Tag("auds") && _audioStream < 0)
                    {
                        // IMA ADPCM only; other audio is ignored
                        if (Get16(data) == 0x11)
                        {
                            _channels = Get16(data + 2);
                            _sampleRate = Get32(data + 4);
                            _blockAlign = Get16(data + 12);
                            _samplesPerBlock = ssize >= 20 && Get16(data + 16) >= 2 ? Get16(data + 18)
                                : (_blockAlign - 4 * _channels) * 2 / _channels + 1;

                            if (_channels >= 1 && _channels <= 2 && _sampleRate && _blockAlign > 4 * _channels)
                            {
                                _hasAudio = true;
                                _audioStream = stream;
                            }
                        }
                    }

                    sp += 8 + ssize + (ssize & 1);
                }
                ++stream;
            }

            pos += 8 + size + (size & 1);
        }

        return true;
    }

    void    AviReader::AddChunk(uint32_t fourcc, uint32_t offset, uint32_t size)
    {
        int stream = StreamOf(fourcc);
        char kind[2] = { char((fourcc >> 16) & 0xFF), char(fourcc >> 24) };

        if (stream == _videoStream && kind[0] == 'd')
        {
            _frames.push_back({offset, size});
            _maxVideoChunk = std::max(_maxVideoChunk, size);
        }
        else if (stream == _audioStream && kind[0] == 'w' && kind[1] == 'b' && size >= _blockAlign)
        {
            AudioChunk chunk;
            chunk.offset = offset;
            chunk.size = size - size % _blockAlign;
            chunk.firstSample = _audioSamples;
            _audio.push_back(chunk);
            _audioSamples += chunk.size / _blockAlign * _samplesPerBlock;
            _maxAudioChunk = std::max(_maxAudioChunk, chunk.size);
        }
    }

    bool    AviReader::ReadIndex(uint32_t start, uint32_t size)
    {
        const uint32_t count = size / 16;

        if (!count)
            return false;

        std::vector<uint8_t> index(count * 16);
        fseek(_file, start, SEEK_SET);
        if (fread(index.data(), 1, index.size(), _file) != index.size())
            return false;

        // Offsets are normally relative to the 'movi' fourcc, but some
        // writers use absolute offsets. Check which one the first entry fits.
        uint32_t base = _moviStart;
        {
            uint8_t c[4];
            fseek(_file, base + Get32(&index[8]), SEEK_SET);
            if (fread(c, 1, 4, _file) != 4 || Get32(c) != Get32(&index[0]))
                base = 0;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t *e = &index[i * 16];
            uint32_t offset = base + Get32(e + 8) + 8;
            uint32_t chunkSize = Get32(e + 12);

            if (offset + chunkSize > _fileSize)
                break;
            AddChunk(Get32(e), offset, chunkSize);
        }
        return !_frames.empty();
    }

    void    AviReader::ScanMovi(uint32_t start, uint32_t end)
    {
        uint32_t pos = start;

        while (pos + 8 <= end)
        {
            uint8_t c[8];

            fseek(_file, pos, SEEK_SET);
            if (fread(c, 1, 8, _file) != 8)
                break;

            uint32_t id = Get32(c);
            uint32_t size = Get32(c + 4);

            if (id == Tag("LIST"))
            {
                pos += 12; // descend into 'rec ' lists
                continue;
            }
            if (id == Tag("idx1") || size > end - pos - 8)
                break; // end of data, or a chunk cut off by the end of the file

            AddChunk(id, pos + 8, size);
            pos += 8 + size + (size & 1);
        }
    }

    int32_t     AviReader::LastRealFrame(uint32_t i) const
    {
        if (i >= _frames.size())
            i = (uint32_t)_frames.size() - 1;

        for (int32_t f = (int32_t)i; f >= 0; --f)
        {
            if (_frames[f].size)
                return f;
        }
        return -1;
    }

    uint32_t    AviReader::AudioChunkAt(uint32_t sample) const
    {
        auto it = std::upper_bound(_audio.begin(), _audio.end(), sample,
            [](uint32_t s, const AudioChunk &c) { return s < c.firstSample; });

        return it == _audio.begin() ? 0 : (uint32_t)(it - _audio.begin() - 1);
    }

    bool    AviReader::Read(const Chunk &chunk, void *out) const
    {
        if (!_file || fseek(_file, chunk.offset, SEEK_SET) != 0)
            return false;
        return fread(out, 1, chunk.size, _file) == chunk.size;
    }
}
