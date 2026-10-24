#include "ImaAdpcm.hpp"

namespace Bingu
{
    static const int16_t    StepTable[89] =
    {
            7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
           19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
           50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
          130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
          337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
          876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
         2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
         5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    };

    static const int8_t     IndexTable[16] =
    {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
    };

    void    ImaAdpcmEncoder::Reset(uint32_t channels)
    {
        _channels = channels < 1 ? 1 : channels > 2 ? 2 : channels;
        for (State &s : _state)
            s = {0, 0};
    }

    uint8_t     ImaAdpcmEncoder::EncodeSample(State &s, int32_t sample)
    {
        int32_t step = StepTable[s.index];
        int32_t diff = sample - s.predictor;
        uint8_t code = 0;

        if (diff < 0)
        {
            code = 8;
            diff = -diff;
        }

        // Same rounding as the decoder, so the predictor stays in sync
        int32_t delta = step >> 3;
        if (diff >= step) { code |= 4; diff -= step; delta += step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; delta += step; }
        step >>= 1;
        if (diff >= step) { code |= 1; delta += step; }

        s.predictor += code & 8 ? -delta : delta;
        if (s.predictor > 32767) s.predictor = 32767;
        if (s.predictor < -32768) s.predictor = -32768;

        s.index += IndexTable[code];
        if (s.index < 0) s.index = 0;
        if (s.index > 88) s.index = 88;

        return code;
    }

    void    ImaAdpcmEncoder::EncodeBlock(const int16_t *samples, uint8_t *out)
    {
        const uint32_t ch = _channels;

        // Block header per channel: first sample stored as-is + step index
        for (uint32_t c = 0; c < ch; ++c)
        {
            State &s = _state[c];

            s.predictor = samples[c];
            out[0] = s.predictor & 0xFF;
            out[1] = (s.predictor >> 8) & 0xFF;
            out[2] = s.index;
            out[3] = 0;
            out += 4;
        }

        // Then groups of 8 samples (4 bytes) per channel, interleaved by
        // channel; low nibble first
        const int16_t *in = samples + ch;

        for (uint32_t group = 0; group < (SamplesPerBlock - 1) / 8; ++group, in += 8 * ch)
        {
            for (uint32_t c = 0; c < ch; ++c)
            {
                State &s = _state[c];

                for (uint32_t i = 0; i < 8; i += 2)
                {
                    uint8_t lo = EncodeSample(s, in[i * ch + c]);
                    uint8_t hi = EncodeSample(s, in[(i + 1) * ch + c]);

                    *out++ = lo | (hi << 4);
                }
            }
        }
    }
}
