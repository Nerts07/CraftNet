#include "chat.hpp"
#include <algorithm>
#include <cstdint>

namespace craft
{
namespace
{
// Reads an NBT length-prefixed string and advances past it.
bool nbtString(wire::Reader &r, std::string &s)
{
    std::uint16_t n;
    if (!r.u16(n) || n > 4096 || n > r.remaining())
    {
        return false;
    }
    s.assign(reinterpret_cast<const char *>(r.data()), n);
    return r.skip(n);
}

// Appends text to the output while keeping it within the display limit.
void append(std::string &out, const std::string &s)
{
    if (out.size() < 512)
    {
        out.append(s, 0, std::min<std::size_t>(s.size(), 512 - out.size()));
    }
}

// Walks one NBT payload value and collects visible chat text from it.
bool payload(wire::Reader &r, std::uint8_t type, const std::string &key, std::string &out, int depth, bool rich = false,
             std::string *style = nullptr)
{
    if (depth > 24)
    {
        return false;
    }
    switch (type)
    {
    case 1:
    {
        std::uint8_t value;
        if (!r.byte(value))
        {
            return false;
        }
        if (rich && style && value && key == "bold")
        {
            *style += "\xc2\xa7l";
        }
        return true;
    }
    case 2:
        return r.skip(2);
    case 3:
    case 5:
        return r.skip(4);
    case 4:
    case 6:
        return r.skip(8);
    case 7:
    case 11:
    case 12:
    {
        std::int32_t n;
        if (!r.i32(n) || n < 0 || n > 65536)
        {
            return false;
        }
        std::size_t scale = type == 7 ? 1 : type == 11 ? 4 : 8;
        return r.skip(static_cast<std::size_t>(n) * scale);
    }
    case 8:
    {
        std::string s;
        if (!nbtString(r, s))
        {
            return false;
        }
        if (key.empty() || key == "text" || key == "fallback")
        {
            append(out, s);
        }
        if (rich && style && key == "color")
        {
            constexpr const char *colors[] = {
                "black",     "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple",  "gold",   "gray",
                "dark_gray", "blue",      "green",      "aqua",      "red",      "light_purple", "yellow", "white"};
            for (int i = 0; i < 16; ++i)
            {
                if (s == colors[i])
                {
                    *style = std::string("\xc2\xa7") + "0123456789abcdef"[i] + *style;
                }
            }
            if (s.size() == 7 && s[0] == '#' && s.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos)
            {
                *style = std::string("\xc2\xa7") + s + *style;
            }
        }
        return true;
    }
    case 9:
    {
        std::uint8_t element;
        std::int32_t n;
        if (!r.byte(element) || !r.i32(n) || n < 0 || n > 256)
        {
            return false;
        }
        for (int i = 0; i < n; ++i)
        {
            if (!payload(r, element, "", out, depth + 1, rich))
            {
                return false;
            }
        }
        return true;
    }
    case 10:
    {
        std::string body, formatting;
        for (int i = 0; i < 512; ++i)
        {
            std::uint8_t child;
            if (!r.byte(child))
            {
                return false;
            }
            if (!child)
            {
                if (rich)
                {
                    append(out, formatting);
                    append(out, body);
                    if (!formatting.empty())
                    {
                        append(out, "\xc2\xa7r");
                    }
                }
                return true;
            }
            std::string name;
            if (!nbtString(r, name))
            {
                return false;
            }
            if (rich &&
                (name == "hoverEvent" || name == "hover_event" || name == "clickEvent" || name == "click_event"))
            {
                std::string ignored;
                if (!payload(r, child, name, ignored, depth + 1))
                {
                    return false;
                }
                continue;
            }
            if (!payload(r, child, name, rich ? body : out, depth + 1, rich, &formatting))
            {
                return false;
            }
        }
        return false;
    }
    default:
        return false;
    }
}
} // namespace

// Decodes a chat component into plain text.
bool readChatText(wire::Reader &reader, std::string &output)
{
    std::uint8_t type;
    if (!reader.byte(type) || !type)
    {
        return false;
    }
    return payload(reader, type, "", output, 0);
}

bool readRichChatText(wire::Reader &reader, std::string &output)
{
    std::uint8_t type;
    return reader.byte(type) && type && payload(reader, type, "", output, 0, true);
}

// Decodes a play-state player chat packet into display text.
bool readPlayerChat(wire::Reader &r, std::string &output)
{
    std::int32_t value, count;
    std::uint8_t present;
    std::string message;
    if (!r.var(value) || !r.skip(16) || !r.var(value) || !r.byte(present) || present > 1 || (present && !r.skip(256)) ||
        !r.str(message, 1024) || !r.skip(16) || !r.var(count) || count < 0 || count > 20)
    {
        return false;
    }
    for (int i = 0; i < count; ++i)
    {
        if (!r.var(value) || value < 0 || (value == 0 && !r.skip(256)))
        {
            return false;
        }
    }
    if (!r.byte(present) || present > 1)
    {
        return false;
    }
    if (present)
    {
        std::string unsignedMessage;
        if (!readChatText(r, unsignedMessage))
        {
            return false;
        }
        if (!unsignedMessage.empty())
        {
            message = unsignedMessage;
        }
    }
    std::int32_t filter;
    if (!r.var(filter) || filter < 0 || filter > 2)
    {
        return false;
    }
    if (filter == 2)
    {
        std::int32_t n;
        if (!r.var(n) || n < 0 || n > 1024 || !r.skip(static_cast<std::size_t>(n)))
        {
            return false;
        }
    }
    if (filter != 0)
    {
        message = "[filtered message]";
    }
    std::string sender;
    if (r.var(value) && value > 0)
    {
        readChatText(r, sender);
    }
    if (!sender.empty())
    {
        output = sender + ": ";
    }
    else
    {
        output = "Player: ";
    }
    append(output, message);
    return true;
}
} // namespace craft
