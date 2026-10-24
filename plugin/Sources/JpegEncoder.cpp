#include "JpegEncoder.hpp"

#include <cstring>

namespace Bingu
{
    //--------------------------------------------------------------------------
    // Standard tables (JPEG spec, Annex K)
    //--------------------------------------------------------------------------

    static const uint8_t    ZigZag[64] =
    {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    };

    static const uint8_t    BaseQuant[2][64] =
    {
        {
            16, 11, 10, 16, 24, 40, 51, 61,  12, 12, 14, 19, 26, 58, 60, 55,
            14, 13, 16, 24, 40, 57, 69, 56,  14, 17, 22, 29, 51, 87, 80, 62,
            18, 22, 37, 56, 68,109,103, 77,  24, 35, 55, 64, 81,104,113, 92,
            49, 64, 78, 87,103,121,120,101,  72, 92, 95, 98,112,100,103, 99,
        },
        {
            17, 18, 24, 47, 99, 99, 99, 99,  18, 21, 26, 66, 99, 99, 99, 99,
            24, 26, 56, 99, 99, 99, 99, 99,  47, 66, 99, 99, 99, 99, 99, 99,
            99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99,
            99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99,
        },
    };

    static const uint8_t    DcBits[2][16] =
    {
        { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },
    };

    static const uint8_t    DcValues[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

    static const uint8_t    AcBits[2][16] =
    {
        { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D },
        { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 },
    };

    static const uint8_t    AcValues[2][162] =
    {
        {
            0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
            0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,
            0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,
            0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
            0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
            0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
            0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
            0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,
            0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,
            0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,
            0xF9,0xFA,
        },
        {
            0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
            0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xA1,0xB1,0xC1,0x09,0x23,0x33,0x52,0xF0,
            0x15,0x62,0x72,0xD1,0x0A,0x16,0x24,0x34,0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,
            0x27,0x28,0x29,0x2A,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,
            0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,
            0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x82,0x83,0x84,0x85,0x86,0x87,
            0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,
            0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,
            0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,
            0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,
            0xF9,0xFA,
        },
    };

    struct HuffTable
    {
        uint16_t    code[256];
        uint8_t     size[256];
    };

    // [0] luma, [1] chroma
    static HuffTable    g_dc[2];
    static HuffTable    g_ac[2];
    static int8_t       g_luma[32768];  ///< RGB555 -> level shifted Y
    static bool         g_tablesReady = false;

    // RGB555 channel -> 8 bit
    static const uint8_t    Expand5[32] =
    {
          0,   8,  16,  24,  33,  41,  49,  57,  66,  74,  82,  90,  99, 107, 115, 123,
        132, 140, 148, 156, 165, 173, 181, 189, 198, 206, 214, 222, 231, 239, 247, 255,
    };

    static void     BuildHuffman(HuffTable &t, const uint8_t *bits, const uint8_t *values)
    {
        uint32_t code = 0;
        uint32_t k = 0;

        std::memset(&t, 0, sizeof(t));
        for (uint32_t len = 1; len <= 16; ++len)
        {
            for (uint32_t i = 0; i < bits[len - 1]; ++i, ++k)
            {
                t.code[values[k]] = code++;
                t.size[values[k]] = len;
            }
            code <<= 1;
        }
    }

    static void     InitTables(void)
    {
        if (g_tablesReady)
            return;

        for (int i = 0; i < 2; ++i)
        {
            BuildHuffman(g_dc[i], DcBits[i], DcValues);
            BuildHuffman(g_ac[i], AcBits[i], AcValues[i]);
        }

        for (uint32_t px = 0; px < 32768; ++px)
        {
            int32_t r = Expand5[(px >> 10) & 31];
            int32_t g = Expand5[(px >> 5) & 31];
            int32_t b = Expand5[px & 31];

            g_luma[px] = ((19595 * r + 38470 * g + 7471 * b + 32768) >> 16) - 128;
        }
        g_tablesReady = true;
    }

    // Chroma is computed from the sum of the 5 bit channels of 2x2 pixels
    // (0 - 124), so the coefficients include the 255 / 124 scale.
    static constexpr int32_t    ChromaCoef(double c)
    {
        return (int32_t)(c * 255.0 / 124.0 * 65536.0 + (c >= 0 ? 0.5 : -0.5));
    }

    // Spreads an RGB555 pixel so four of them can be summed without the
    // channels overflowing into each other: B bits 0-6, R bits 10-16, G bits 21-27.
    static inline uint32_t  Spread(uint32_t px)
    {
        return (px & 0x7C1F) | ((px & 0x03E0) << 16);
    }

    void    JpegEncoder::SetQuality(int quality)
    {
        // Scale factors of the AAN DCT, folded into the quantization step
        static const double aan[8] =
        {
            1.0, 1.387039845, 1.306562965, 1.175875602,
            1.0, 0.785694958, 0.541196100, 0.275899379,
        };

        InitTables();

        if (quality < 1) quality = 1;
        if (quality > 100) quality = 100;

        int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;

        for (int t = 0; t < 2; ++t)
        {
            for (int i = 0; i < 64; ++i)
            {
                int q = (BaseQuant[t][i] * scale + 50) / 100;

                if (q < 1) q = 1;
                if (q > 255) q = 255;
                _qtable[t][i] = q;

                double divisor = q * aan[i >> 3] * aan[i & 7] * 8.0;
                _recip[t][i] = (uint32_t)(65536.0 / divisor + 0.5);
            }
        }
    }

    //--------------------------------------------------------------------------
    // Forward DCT (AAN, 8 bit fixed point, same as libjpeg's jfdctfst)
    //--------------------------------------------------------------------------

    #define FIX_0_382683433  98
    #define FIX_0_541196100  139
    #define FIX_0_707106781  181
    #define FIX_1_306562965  334
    #define MULTIPLY(v, c)   (((v) * (c)) >> 8)

    static inline void  Dct1D(int32_t *d, int step)
    {
        int32_t tmp0 = d[0 * step] + d[7 * step];
        int32_t tmp7 = d[0 * step] - d[7 * step];
        int32_t tmp1 = d[1 * step] + d[6 * step];
        int32_t tmp6 = d[1 * step] - d[6 * step];
        int32_t tmp2 = d[2 * step] + d[5 * step];
        int32_t tmp5 = d[2 * step] - d[5 * step];
        int32_t tmp3 = d[3 * step] + d[4 * step];
        int32_t tmp4 = d[3 * step] - d[4 * step];

        // Even part
        int32_t tmp10 = tmp0 + tmp3;
        int32_t tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2;
        int32_t tmp12 = tmp1 - tmp2;

        d[0 * step] = tmp10 + tmp11;
        d[4 * step] = tmp10 - tmp11;

        int32_t z1 = MULTIPLY(tmp12 + tmp13, FIX_0_707106781);
        d[2 * step] = tmp13 + z1;
        d[6 * step] = tmp13 - z1;

        // Odd part
        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        int32_t z5 = MULTIPLY(tmp10 - tmp12, FIX_0_382683433);
        int32_t z2 = MULTIPLY(tmp10, FIX_0_541196100) + z5;
        int32_t z4 = MULTIPLY(tmp12, FIX_1_306562965) + z5;
        int32_t z3 = MULTIPLY(tmp11, FIX_0_707106781);

        int32_t z11 = tmp7 + z3;
        int32_t z13 = tmp7 - z3;

        d[5 * step] = z13 + z2;
        d[3 * step] = z13 - z2;
        d[1 * step] = z11 + z4;
        d[7 * step] = z11 - z4;
    }

    static void     ForwardDct(int32_t *block)
    {
        for (int row = 0; row < 8; ++row)
            Dct1D(block + row * 8, 1);
        for (int col = 0; col < 8; ++col)
            Dct1D(block + col, 8);
    }

    //--------------------------------------------------------------------------
    // Entropy coding
    //--------------------------------------------------------------------------

    struct BitWriter
    {
        uint8_t    *out;
        uint32_t    buffer;
        uint32_t    count;

        inline void Put(uint32_t bits, uint32_t size)
        {
            buffer = (buffer << size) | bits;
            count += size;

            while (count >= 8)
            {
                uint8_t byte = buffer >> (count - 8);

                *out++ = byte;
                if (byte == 0xFF)
                    *out++ = 0; // byte stuffing
                count -= 8;
            }
        }

        void        Flush(void)
        {
            if (count)
                Put((1u << (8 - count)) - 1, 8 - count); // pad with 1 bits
        }
    };

    static inline uint32_t  BitLength(uint32_t v)
    {
        return v ? 32 - __builtin_clz(v) : 0;
    }

    static inline int32_t   Quantize(int32_t c, uint32_t recip)
    {
        int32_t v = (int32_t)(((uint32_t)(c < 0 ? -c : c) * recip + 32768) >> 16);
        return c < 0 ? -v : v;
    }

    static void     EncodeBlock(BitWriter &bw, int32_t *block, const uint32_t *recip, int32_t &prevDc, int table)
    {
        const HuffTable &dc = g_dc[table];
        const HuffTable &ac = g_ac[table];

        ForwardDct(block);

        // DC: difference to the previous block of the same component
        int32_t value = Quantize(block[0], recip[0]);
        int32_t diff = value - prevDc;
        prevDc = value;

        uint32_t mag = diff < 0 ? -diff : diff;
        uint32_t nbits = BitLength(mag);

        bw.Put(dc.code[nbits], dc.size[nbits]);
        if (nbits)
            bw.Put((diff < 0 ? diff - 1 : diff) & ((1u << nbits) - 1), nbits);

        // AC: run-length of zeros + value, in zigzag order
        uint32_t run = 0;

        for (int k = 1; k < 64; ++k)
        {
            const uint32_t n = ZigZag[k];
            const int32_t v = Quantize(block[n], recip[n]);

            if (!v)
            {
                ++run;
                continue;
            }

            while (run > 15)
            {
                bw.Put(ac.code[0xF0], ac.size[0xF0]);
                run -= 16;
            }

            mag = v < 0 ? -v : v;
            nbits = BitLength(mag);

            uint32_t symbol = (run << 4) | nbits;
            bw.Put(ac.code[symbol], ac.size[symbol]);
            bw.Put((v < 0 ? v - 1 : v) & ((1u << nbits) - 1), nbits);
            run = 0;
        }

        if (run)
            bw.Put(ac.code[0x00], ac.size[0x00]); // end of block
    }

    //--------------------------------------------------------------------------
    // File structure
    //--------------------------------------------------------------------------

    static uint8_t     *PutMarker(uint8_t *p, uint8_t marker, uint32_t length)
    {
        *p++ = 0xFF;
        *p++ = marker;
        *p++ = length >> 8;
        *p++ = length & 0xFF;
        return p;
    }

    static uint8_t     *PutHuffman(uint8_t *p, uint8_t id, const uint8_t *bits, const uint8_t *values, uint32_t count)
    {
        *p++ = id;
        std::memcpy(p, bits, 16);
        p += 16;
        std::memcpy(p, values, count);
        return p + count;
    }

    // Bytes needed for one MCU in the worst case (6 blocks, every coefficient
    // coded with the longest code, plus byte stuffing)
    static constexpr uint32_t   WorstCaseMcu = 6 * 64 * 4 * 2;
    static constexpr uint32_t   HeaderSize = 2 + 18 + 134 + 19 + 420 + 14;

    uint32_t    JpegEncoder::Encode(const uint16_t *pixels, uint32_t width, uint32_t height, uint8_t *out, uint32_t capacity)
    {
        if (capacity < HeaderSize + WorstCaseMcu + 2 || !width || !height)
            return 0;

        uint8_t *p = out;
        uint8_t *end = out + capacity;

        // SOI + JFIF
        *p++ = 0xFF; *p++ = 0xD8;
        p = PutMarker(p, 0xE0, 16);
        std::memcpy(p, "JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 14);
        p += 14;

        // Quantization tables, zigzag order
        p = PutMarker(p, 0xDB, 2 + 65 * 2);
        for (int t = 0; t < 2; ++t)
        {
            *p++ = t;
            for (int k = 0; k < 64; ++k)
                *p++ = _qtable[t][ZigZag[k]];
        }

        // Frame header: 3 components, luma sampled 2x2
        p = PutMarker(p, 0xC0, 17);
        *p++ = 8;
        *p++ = height >> 8; *p++ = height & 0xFF;
        *p++ = width >> 8;  *p++ = width & 0xFF;
        *p++ = 3;
        *p++ = 1; *p++ = 0x22; *p++ = 0;
        *p++ = 2; *p++ = 0x11; *p++ = 1;
        *p++ = 3; *p++ = 0x11; *p++ = 1;

        // Huffman tables (always included: AVI players differ on defaults)
        p = PutMarker(p, 0xC4, 2 + 4 * 17 + 12 * 2 + 162 * 2);
        p = PutHuffman(p, 0x00, DcBits[0], DcValues, 12);
        p = PutHuffman(p, 0x10, AcBits[0], AcValues[0], 162);
        p = PutHuffman(p, 0x01, DcBits[1], DcValues, 12);
        p = PutHuffman(p, 0x11, AcBits[1], AcValues[1], 162);

        // Start of scan
        p = PutMarker(p, 0xDA, 12);
        *p++ = 3;
        *p++ = 1; *p++ = 0x00;
        *p++ = 2; *p++ = 0x11;
        *p++ = 3; *p++ = 0x11;
        *p++ = 0; *p++ = 63; *p++ = 0;

        BitWriter bw{p, 0, 0};
        int32_t prevDc[3] = {0, 0, 0};
        int32_t y[4][64];
        int32_t cb[64];
        int32_t cr[64];
        const uint16_t *rows[16];
        uint32_t cols[16];

        for (uint32_t my = 0; my < height; my += 16)
        {
            // JPEG is top-down, the input is bottom-up. Rows/columns past the
            // edge repeat the last pixel.
            for (uint32_t j = 0; j < 16; ++j)
            {
                uint32_t row = my + j < height ? my + j : height - 1;
                rows[j] = pixels + (height - 1 - row) * width;
            }

            for (uint32_t mx = 0; mx < width; mx += 16)
            {
                if ((uint32_t)(end - bw.out) < WorstCaseMcu + 2)
                    return 0;

                for (uint32_t i = 0; i < 16; ++i)
                    cols[i] = mx + i < width ? mx + i : width - 1;

                // RGB -> YCbCr, chroma averaged over 2x2 pixels
                for (uint32_t j = 0; j < 16; j += 2)
                {
                    const uint16_t *row0 = rows[j];
                    const uint16_t *row1 = rows[j + 1];

                    for (uint32_t i = 0; i < 16; i += 2)
                    {
                        uint32_t p0 = row0[cols[i]] & 0x7FFF, p1 = row0[cols[i + 1]] & 0x7FFF;
                        uint32_t p2 = row1[cols[i]] & 0x7FFF, p3 = row1[cols[i + 1]] & 0x7FFF;
                        int32_t *luma = y[(j >> 3) * 2 + (i >> 3)] + (j & 7) * 8 + (i & 7);

                        luma[0] = g_luma[p0];
                        luma[1] = g_luma[p1];
                        luma[8] = g_luma[p2];
                        luma[9] = g_luma[p3];

                        uint32_t sum = Spread(p0) + Spread(p1) + Spread(p2) + Spread(p3);
                        int32_t r = (sum >> 10) & 0x7F;
                        int32_t g = (sum >> 21) & 0x7F;
                        int32_t b = sum & 0x7F;
                        uint32_t c = (j >> 1) * 8 + (i >> 1);

                        cb[c] = (ChromaCoef(-0.168736) * r + ChromaCoef(-0.331264) * g + ChromaCoef(0.5) * b + 32768) >> 16;
                        cr[c] = (ChromaCoef(0.5) * r + ChromaCoef(-0.418688) * g + ChromaCoef(-0.081312) * b + 32768) >> 16;
                    }
                }

                for (int b = 0; b < 4; ++b)
                    EncodeBlock(bw, y[b], _recip[0], prevDc[0], 0);
                EncodeBlock(bw, cb, _recip[1], prevDc[1], 1);
                EncodeBlock(bw, cr, _recip[1], prevDc[2], 1);
            }
        }

        bw.Flush();
        p = bw.out;
        *p++ = 0xFF; *p++ = 0xD9; // EOI
        return p - out;
    }
}
