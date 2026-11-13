#pragma once
#include <raylib.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace craft
{
inline void textFormatCode(const char *&text, Color &color, Color fallback, bool &bold)
{
    constexpr unsigned colors[] = {0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xffaa00, 0xaaaaaa,
                                    0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff};
    const auto hex = [](char c)
    {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    const char code = *text++;
    const auto apply = [&](unsigned rgb)
    {
        color = {static_cast<unsigned char>(rgb >> 16), static_cast<unsigned char>(rgb >> 8),
                 static_cast<unsigned char>(rgb), fallback.a};
    };
    if (const int value = hex(code); value >= 0)
    {
        apply(colors[value]);
        bold = false;
    }
    else if (code == 'l')
    {
        bold = true;
    }
    else if (code == 'r')
    {
        color = fallback;
        bold = false;
    }
    else if (code == '#')
    {
        unsigned rgb = 0;
        for (int i = 0; i < 6; ++i)
        {
            const int value = hex(text[i]);
            if (value < 0)
            {
                return;
            }
            rgb = (rgb << 4) | static_cast<unsigned>(value);
        }
        text += 6;
        apply(rgb);
    }
}

inline int gameGlyphIndex(Font font, int code)
{
    struct GlyphCache
    {
        const GlyphInfo *glyphs = nullptr;
        unsigned texture = 0;
        int count = 0;
        std::array<std::uint16_t, 0x500> indices{};
    };
    static GlyphCache cache;
    if (cache.glyphs != font.glyphs || cache.texture != font.texture.id || cache.count != font.glyphCount)
    {
        cache.glyphs = font.glyphs;
        cache.texture = font.texture.id;
        cache.count = font.glyphCount;
        cache.indices.fill(static_cast<std::uint16_t>(GetGlyphIndex(font, '?')));
        for (int i = 0; i < font.glyphCount && i < 65536; ++i)
        {
            const unsigned code = static_cast<unsigned>(font.glyphs[i].value);
            if (code < cache.indices.size())
            {
                cache.indices[code] = static_cast<std::uint16_t>(i);
            }
        }
    }
    return static_cast<unsigned>(code) < cache.indices.size() ? cache.indices[static_cast<unsigned>(code)] :
                                                                 GetGlyphIndex(font, code);
}

inline float gameGlyphAdvance(Font font, int index, bool bold)
{
    const auto advance = font.glyphs[index].advanceX;
    return (advance ? static_cast<float>(advance) : font.recs[index].width) + (bold ? 1 : 0);
}

inline float gameTextWidth(Font font, const char *text, float size)
{
    float width = 0;
    bool bold = false;
    Color unused = WHITE;
    while (*text)
    {
        int bytes = 0;
        const int code = GetCodepointNext(text, &bytes);
        if (bytes <= 0 || code == '\n')
        {
            break;
        }
        text += bytes;
        if (code == 0xa7 && *text)
        {
            textFormatCode(text, unused, WHITE, bold);
            continue;
        }
        width += gameGlyphAdvance(font, gameGlyphIndex(font, code), bold);
    }
    return width * size / font.baseSize;
}

struct WrappedTextLine
{
    std::string text;
    Color color;
    bool bold;
};

inline void wrapGameText(Font font, const std::string &text, float size, float width, Color tint,
                         std::vector<WrappedTextLine> &lines)
{
    lines.clear();
    const float limit = width * font.baseSize / size;
    const char *cursor = text.c_str();
    Color color = tint;
    bool bold = false;
    while (*cursor)
    {
        const char *start = cursor;
        const Color startColor = color;
        const bool startBold = bold;
        const char *wordBreak = nullptr;
        Color breakColor{};
        bool breakBold = false;
        const char *end = cursor;
        float advance = 0;
        while (*cursor)
        {
            int bytes = 0;
            const int code = GetCodepointNext(cursor, &bytes);
            if (code == '\n')
            {
                cursor += bytes;
                break;
            }
            if (code == 0xa7 && cursor[bytes])
            {
                cursor += bytes;
                textFormatCode(cursor, color, tint, bold);
                end = cursor;
                continue;
            }
            const bool space = code == ' ' || code == '\t';
            if (space && advance > 0)
            {
                wordBreak = cursor;
                breakColor = color;
                breakBold = bold;
            }
            const float glyph = gameGlyphAdvance(font, gameGlyphIndex(font, code), bold);
            if (advance > 0 && advance + glyph > limit)
            {
                if (wordBreak)
                {
                    end = cursor = wordBreak;
                    color = breakColor;
                    bold = breakBold;
                    while (*cursor == ' ' || *cursor == '\t')
                    {
                        ++cursor;
                    }
                }
                break;
            }
            advance += glyph;
            cursor += bytes;
            end = cursor;
        }
        lines.push_back({std::string(start, end), startColor, startBold});
    }
}

// Scissoring alone still submits every hidden glyph. Stop at the visible edge
// and look up the common Latin/Cyrillic glyphs without Raylib's linear searches.
inline void drawClippedText(Font font, const char *text, Vector2 position, float size, float width, Color tint,
                            const WrappedTextLine *initial = nullptr)
{
    if (!font.texture.id)
    {
        font = GetFontDefault();
    }
    if (!font.glyphs || !font.recs || font.baseSize <= 0 || width <= 0)
    {
        return;
    }
    const float scale = size / font.baseSize;
    const float padding = static_cast<float>(font.glyphPadding);
    float offset = 0;
    Color color = initial ? initial->color : tint;
    bool bold = initial && initial->bold;
    while (*text && offset < width)
    {
        int bytes = 0;
        const int code = GetCodepointNext(text, &bytes);
        if (bytes <= 0 || code == '\n')
        {
            break;
        }
        text += bytes;
        if (code == 0xa7 && *text)
        {
            textFormatCode(text, color, tint, bold);
            continue;
        }
        const int index = gameGlyphIndex(font, code);
        const auto &glyph = font.glyphs[index];
        const auto &rec = font.recs[index];
        if (code != ' ' && code != '\t')
        {
            const Rectangle source{rec.x - padding, rec.y - padding, rec.width + padding * 2, rec.height + padding * 2};
            const Rectangle destination{position.x + offset + (glyph.offsetX - padding) * scale,
                                        position.y + (glyph.offsetY - padding) * scale, source.width * scale,
                                        source.height * scale};
            DrawTexturePro(font.texture, source, destination, {0, 0}, 0, color);
            if (bold)
            {
                auto shifted = destination;
                shifted.x += scale;
                DrawTexturePro(font.texture, source, shifted, {0, 0}, 0, color);
            }
        }
        offset += gameGlyphAdvance(font, index, bold) * scale;
    }
}
} // namespace craft
