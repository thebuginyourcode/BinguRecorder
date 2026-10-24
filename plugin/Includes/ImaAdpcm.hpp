#ifndef BINGU_IMAADPCM_HPP
#define BINGU_IMAADPCM_HPP

#include <cstdint>

namespace Bingu
{
    // IMA ADPCM encoder producing WAV-style blocks (4 bits per sample, about
    // 4x smaller than 16-bit PCM). Each block starts with a header per
    // channel, so a block can be decoded on its own.
    class ImaAdpcmEncoder
    {
    public:
        // Bytes per block per channel; 512 gives 1017 samples per block
        static constexpr uint32_t   BlockBytesPerChannel = 512;
        static constexpr uint32_t   SamplesPerBlock = (BlockBytesPerChannel - 4) * 2 + 1;

        explicit ImaAdpcmEncoder(uint32_t channels = 2) { Reset(channels); }

        void        Reset(uint32_t channels);

        uint32_t    Channels(void) const { return _channels; }
        uint32_t    BlockSize(void) const { return BlockBytesPerChannel * _channels; }

        // Encodes SamplesPerBlock interleaved samples per channel into one
        // BlockSize() bytes block.
        void        EncodeBlock(const int16_t *samples, uint8_t *out);

    private:
        struct State
        {
            int32_t     predictor;
            int32_t     index;
        };

        uint8_t     EncodeSample(State &s, int32_t sample);

        uint32_t    _channels{2};
        State       _state[2];
    };
}

#endif
