#include "FrameBlit.hpp"

#include <cstring>

namespace Bingu
{
    static constexpr uint32_t   ScreenHeight = 240;

    static Region   Centered(uint32_t width, uint32_t height, uint32_t scale, uint32_t screenWidth)
    {
        Region r;
        uint32_t w = width * scale > screenWidth ? screenWidth : width * scale;
        uint32_t h = height * scale > ScreenHeight ? ScreenHeight : height * scale;

        r.visible = true;
        r.width = w / scale;
        r.height = h / scale;
        r.srcX = (width - r.width) / 2;
        r.srcY = (height - r.height) / 2;
        r.dstX = (screenWidth - r.width * scale) / 2;
        r.dstY = (ScreenHeight - r.height * scale) / 2;
        return r;
    }

    Layout  ChooseLayout(uint32_t width, uint32_t height)
    {
        Layout l;

        l.scale = width <= 200 && height <= 240 ? 2 : 1;
        l.top.visible = false;
        l.bottom.visible = false;

        const uint32_t w = width * l.scale;
        const uint32_t h = height * l.scale;

        if (w == 400 && h == 480)
        {
            // Both screens: top half -> top screen, centered 320 columns of
            // the bottom half -> bottom screen
            l.top = { true, 0, 0, width, height / 2, 0, 0 };
            l.bottom = { true, 40 / l.scale, height / 2, 320 / l.scale, height / 2, 0, 0 };
        }
        else if (w == 320 && h == 240)
            l.bottom = { true, 0, 0, width, height, 0, 0 };
        else
            l.top = Centered(width, height, l.scale, 400);

        return l;
    }

    template <FbFormat Format, uint32_t Scale>
    static void     BlitImpl(const uint8_t *bgr, uint32_t srcWidth, const Region &r, const FbTarget &dst)
    {
        const uint32_t bpp = Format == FbFormat::Bgr8 ? 3 : 2;
        const uint32_t columns = r.width * Scale;
        const uint32_t rows = r.height * Scale;

        for (uint32_t x = 0; x < columns; ++x)
        {
            const uint8_t *src = bgr + ((size_t)r.srcY * srcWidth + r.srcX + x / Scale) * 3;
            // Pixel (x, y) of the screen is at column x, (239 - y) from the column start
            uint8_t *out = dst.pixels + ((size_t)(r.dstX + x) * ScreenHeight + (ScreenHeight - 1 - r.dstY)) * bpp;

            for (uint32_t y = 0; y < rows; ++y, out -= bpp)
            {
                const uint8_t *p = src + (size_t)(y / Scale) * srcWidth * 3;

                if (Format == FbFormat::Bgr8)
                {
                    out[0] = p[0];
                    out[1] = p[1];
                    out[2] = p[2];
                }
                else
                {
                    uint16_t c = ((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3);
                    std::memcpy(out, &c, 2);
                }
            }
        }
    }

    void    Blit(const uint8_t *bgr, uint32_t srcWidth, const Region &region, uint32_t scale, const FbTarget &dst)
    {
        if (!region.visible)
            return;

        if (dst.format == FbFormat::Bgr8)
        {
            if (scale == 2) BlitImpl<FbFormat::Bgr8, 2>(bgr, srcWidth, region, dst);
            else            BlitImpl<FbFormat::Bgr8, 1>(bgr, srcWidth, region, dst);
        }
        else
        {
            if (scale == 2) BlitImpl<FbFormat::Rgb565, 2>(bgr, srcWidth, region, dst);
            else            BlitImpl<FbFormat::Rgb565, 1>(bgr, srcWidth, region, dst);
        }
    }

    void    Rgb555ToBgr24(const uint16_t *src, uint32_t width, uint32_t height, uint8_t *dst)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint16_t *row = src + (size_t)(height - 1 - y) * width;

            for (uint32_t x = 0; x < width; ++x, dst += 3)
            {
                uint16_t c = row[x];
                uint8_t r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;

                dst[0] = (b << 3) | (b >> 2);
                dst[1] = (g << 3) | (g >> 2);
                dst[2] = (r << 3) | (r >> 2);
            }
        }
    }

    void    Clear(const FbTarget &dst)
    {
        std::memset(dst.pixels, 0, (size_t)dst.width * ScreenHeight * (dst.format == FbFormat::Bgr8 ? 3 : 2));
    }
}
