#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace craft
{
inline void appendUtf8(std::string &text, unsigned codepoint)
{
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
    {
        return;
    }
    if (codepoint < 0x80)
    {
        text.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint < 0x800)
    {
        text.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 63)));
    }
    else if (codepoint < 0x10000)
    {
        text.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 63)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 63)));
    }
    else
    {
        text.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 63)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 63)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 63)));
    }
}

inline void popUtf8(std::string &text)
{
    while (!text.empty())
    {
        const unsigned char c = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((c & 0xc0) != 0x80)
        {
            break;
        }
    }
}

inline std::string displayText(const std::string &input, std::size_t limit)
{
    std::string text;
    text.reserve(std::min(input.size(), limit));
    for (std::size_t i = 0; i < input.size() && text.size() < limit; ++i)
    {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == 0xc2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xa7)
        {
            i += std::min<std::size_t>(2, input.size() - 1 - i);
        }
        else if (c >= 32 || c == '\n')
        {
            text.push_back(static_cast<char>(c));
        }
    }
    // A cut must not leave the leading bytes of a UTF-8 character behind.
    if (!text.empty() && text.size() < input.size())
    {
        std::size_t start = text.size() - 1;
        while (start && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
        {
            --start;
        }
        const auto first = static_cast<unsigned char>(text[start]);
        const std::size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        if (start + length > text.size())
        {
            text.resize(start);
        }
    }
    return text;
}
} // namespace craft
