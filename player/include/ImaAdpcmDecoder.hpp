#ifndef BINGU_IMAADPCMDECODER_HPP
#define BINGU_IMAADPCMDECODER_HPP

#include <cstdint>

namespace Bingu
{
    // Decodes one WAV-style IMA ADPCM block (as written by BinguRecorder)
    // into `samplesPerBlock` interleaved 16-bit samples per channel.
    void    DecodeImaAdpcmBlock(const uint8_t *block, uint32_t blockAlign, uint32_t channels,
                                uint32_t samplesPerBlock, int16_t *out);
}

#endif
