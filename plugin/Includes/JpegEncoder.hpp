#ifndef BINGU_JPEGENCODER_HPP
#define BINGU_JPEGENCODER_HPP

#include <cstdint>

namespace Bingu
{
    // Small baseline JPEG encoder (YCbCr 4:2:0, standard Huffman tables) made
    // to be fast on the 3DS's ARM11: integer AAN DCT, no floating point.
    //
    // Input is the recorder's frame format: RGB555 pixels, bottom-up rows.
    // Any width/height works; partial 16x16 blocks repeat the edge pixels.
    class JpegEncoder
    {
    public:
        explicit JpegEncoder(int quality = 75) { SetQuality(quality); }

        // 1 (smallest) - 100 (best), same scale as libjpeg
        void        SetQuality(int quality);

        // Returns the size of the JPEG written to `out`, or 0 if it didn't fit.
        uint32_t    Encode(const uint16_t *pixels, uint32_t width, uint32_t height, uint8_t *out, uint32_t capacity);

    private:
        uint8_t     _qtable[2][64];     ///< Quantization tables, natural order
        uint32_t    _recip[2][64];      ///< 65536 / (quant * AAN scale), natural order
    };
}

#endif
