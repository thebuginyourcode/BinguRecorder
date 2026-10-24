#ifndef BINGU_FRAMEBLIT_HPP
#define BINGU_FRAMEBLIT_HPP

#include <cstdint>

namespace Bingu
{
    enum class FbFormat
    {
        Bgr8,   ///< 3 bytes per pixel: B, G, R
        Rgb565, ///< What the libctru text console uses
    };

    // A 3DS framebuffer: stored rotated, one column of 240 pixels after the
    // other, each column starting with the bottom pixel.
    struct FbTarget
    {
        uint8_t    *pixels;
        uint32_t    width;      ///< 400 (top) or 320 (bottom)
        FbFormat    format;
    };

    // Part of the video shown on one screen, in video pixels
    struct Region
    {
        bool        visible;
        uint32_t    srcX, srcY, width, height;
        uint32_t    dstX, dstY;     ///< On the screen, in screen pixels
    };

    struct Layout
    {
        uint32_t    scale;          ///< 1, or 2 for half-size recordings
        Region      top;
        Region      bottom;
    };

    // Puts top screen recordings on the top screen, bottom screen recordings
    // on the bottom screen, and "both screens" recordings (top above bottom)
    // back on both. Half-size recordings are scaled up 2x. Anything else is
    // centered (and cropped if needed) on the top screen.
    Layout  ChooseLayout(uint32_t width, uint32_t height);

    // Copies a region of a BGR24 picture (top-down rows) to a framebuffer
    void    Blit(const uint8_t *bgr, uint32_t srcWidth, const Region &region, uint32_t scale, const FbTarget &dst);

    // BinguRecorder's raw frames (RGB555, bottom-up rows) -> BGR24 top-down
    void    Rgb555ToBgr24(const uint16_t *src, uint32_t width, uint32_t height, uint8_t *dst);

    void    Clear(const FbTarget &dst);
}

#endif
