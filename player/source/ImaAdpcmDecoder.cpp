#include "ImaAdpcmDecoder.hpp"

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

    static inline int16_t   DecodeNibble(int32_t &predictor, int32_t &index, uint8_t code)
    {
        int32_t step = StepTable[index];
        int32_t delta = step >> 3;

        if (code & 4) delta += step;
        if (code & 2) delta += step >> 1;
        if (code & 1) delta += step >> 2;

        predictor += code & 8 ? -delta : delta;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;

        index += IndexTable[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;

        return (int16_t)predictor;
    }

    void    DecodeImaAdpcmBlock(const uint8_t *block, uint32_t blockAlign, uint32_t channels,
                                uint32_t samplesPerBlock, int16_t *out)
    {
        int32_t predictor[2], index[2];

        // Header per channel: first sample + step index
        for (uint32_t c = 0; c < channels; ++c)
        {
            predictor[c] = (int16_t)(block[0] | (block[1] << 8));
            index[c] = block[2] > 88 ? 88 : block[2];
            out[c] = (int16_t)predictor[c];
            block += 4;
        }

        // Groups of 8 samples (4 bytes) per channel, low nibble first
        const uint32_t groups = (samplesPerBlock - 1) / 8;
        const uint32_t available = (blockAlign - 4 * channels) / (4 * channels);
        int16_t *o = out + channels;

        for (uint32_t g = 0; g < groups && g < available; ++g, o += 8 * channels)
        {
            for (uint32_t c = 0; c < channels; ++c)
            {
                for (uint32_t i = 0; i < 8; i += 2)
                {
                    uint8_t byte = *block++;

                    o[i * channels + c] = DecodeNibble(predictor[c], index[c], byte & 0xF);
                    o[(i + 1) * channels + c] = DecodeNibble(predictor[c], index[c], byte >> 4);
                }
            }
        }
    }
}
