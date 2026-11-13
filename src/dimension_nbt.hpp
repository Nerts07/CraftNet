#pragma once
#include "wire.hpp"
#include <cstring>

namespace craft
{
// Bounds-checked NBT traversal; retain only the dimension's vertical bounds.
inline bool dimensionValue(wire::Reader &r, unsigned type, int depth, int &minY, int &height, unsigned &found,
                           unsigned key = 0)
{
    if (depth > 24)
    {
        return false;
    }
    if (type == 3)
    {
        std::int32_t value;
        if (!r.i32(value))
        {
            return false;
        }
        if (key == 1)
        {
            minY = value;
            found |= 1;
        }
        if (key == 2)
        {
            height = value;
            found |= 2;
        }
        return true;
    }
    if (type == 10)
    {
        for (unsigned i = 0; i < 8192; ++i)
        {
            std::uint8_t child;
            std::uint16_t length;
            if (!r.byte(child))
            {
                return false;
            }
            if (!child)
            {
                return true;
            }
            if (!r.u16(length) || length > r.remaining())
            {
                return false;
            }
            unsigned field = 0;
            if (!depth && length == 5 && !std::memcmp(r.data(), "min_y", 5))
            {
                field = 1;
            }
            if (!depth && length == 6 && !std::memcmp(r.data(), "height", 6))
            {
                field = 2;
            }
            if (!r.skip(length) || !dimensionValue(r, child, depth + 1, minY, height, found, field))
            {
                return false;
            }
        }
        return false;
    }
    if (type == 9)
    {
        std::uint8_t child;
        std::int32_t count;
        if (!r.byte(child) || !r.i32(count) || count < 0 || count > 8192)
        {
            return false;
        }
        for (int i = 0; i < count; ++i)
        {
            if (!dimensionValue(r, child, depth + 1, minY, height, found))
            {
                return false;
            }
        }
        return true;
    }
    if (type == 7 || type == 11 || type == 12)
    {
        std::int32_t count;
        if (!r.i32(count) || count < 0 || count > 1048576)
        {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * (type == 7 ? 1 : type == 11 ? 4 : 8));
    }
    if (type == 8)
    {
        std::uint16_t length;
        return r.u16(length) && r.skip(length);
    }
    const unsigned bytes = type == 1 ? 1 : type == 2 ? 2 : type == 5 ? 4 : type == 4 || type == 6 ? 8 : 0;
    return bytes && r.skip(bytes);
}
} // namespace craft
