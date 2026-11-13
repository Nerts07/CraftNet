#pragma once
#include "protocol.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace craft::wire
{
// Bounds-checked cursor for reading little-endian/network-order packet fields.
class Reader
{
  public:
    // Stores the packet buffer and starts reading at offset zero.
    Reader(const std::uint8_t *data, std::size_t size) : data_(data), size_(size)
    {
    }
    // Reads one unsigned byte.
    bool byte(std::uint8_t &v)
    {
        if (remaining() < 1)
        {
            return false;
        }
        v = data_[at_++];
        return true;
    }
    // Advances the cursor by the requested byte count.
    bool skip(std::size_t n)
    {
        if (n > remaining())
        {
            return false;
        }
        at_ += n;
        return true;
    }
    // Reads a protocol VarInt.
    bool var(std::int32_t &v)
    {
        return protocol::readVarInt(data_, size_, at_, v) == protocol::DecodeResult::Packet;
    }
    // Reads a protocol VarLong.
    bool varlong(std::uint64_t &v)
    {
        std::uint64_t result = 0;
        auto at = at_;
        for (unsigned i = 0; i < 10; ++i)
        {
            if (at >= size_)
            {
                return false;
            }
            auto c = data_[at++];
            if (i == 9 && (c & 0xfe))
            {
                return false;
            }
            result |= std::uint64_t(c & 0x7f) << (7 * i);
            if (!(c & 0x80))
            {
                at_ = at;
                v = result;
                return true;
            }
        }
        return false;
    }
    // Reads a big-endian unsigned 16-bit integer.
    bool u16(std::uint16_t &v)
    {
        if (remaining() < 2)
        {
            return false;
        }
        v = (std::uint16_t(data_[at_]) << 8) | data_[at_ + 1];
        at_ += 2;
        return true;
    }
    // Reads a big-endian signed 32-bit integer.
    bool i32(std::int32_t &v)
    {
        std::uint64_t x;
        if (!unsignedBytes(x, 4))
        {
            return false;
        }
        v = static_cast<std::int32_t>(x);
        return true;
    }
    // Reads a big-endian unsigned 64-bit integer.
    bool u64(std::uint64_t &v)
    {
        return unsignedBytes(v, 8);
    }
    // Reads a big-endian 32-bit float.
    bool f32(float &v)
    {
        std::uint64_t x;
        if (!unsignedBytes(x, 4))
        {
            return false;
        }
        auto y = static_cast<std::uint32_t>(x);
        std::memcpy(&v, &y, 4);
        return true;
    }
    // Reads a big-endian 64-bit double.
    bool f64(double &v)
    {
        std::uint64_t x;
        if (!u64(x))
        {
            return false;
        }
        std::memcpy(&v, &x, 8);
        return true;
    }
    // Reads a length-prefixed UTF-8 string up to the given maximum.
    bool str(std::string &v, std::size_t max = 32767)
    {
        std::int32_t n = 0;
        if (!var(n) || n < 0 || static_cast<std::size_t>(n) > max || static_cast<std::size_t>(n) > remaining())
        {
            return false;
        }
        v.assign(reinterpret_cast<const char *>(data_ + at_), static_cast<std::size_t>(n));
        at_ += static_cast<std::size_t>(n);
        return true;
    }
    // Returns the current read pointer.
    const std::uint8_t *data() const
    {
        return data_ + at_;
    }
    // Returns the number of unread bytes.
    std::size_t remaining() const
    {
        return size_ - at_;
    }

  private:
    // Reads a big-endian integer of the requested byte width.
    bool unsignedBytes(std::uint64_t &value, std::size_t n)
    {
        if (n > remaining())
        {
            return false;
        }
        value = 0;
        while (n--)
        {
            value = (value << 8) | data_[at_++];
        }
        return true;
    }
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t at_ = 0;
};

using Bytes = std::vector<std::uint8_t>;

// Appends a protocol VarInt to a byte buffer.
inline void var(Bytes &out, std::int32_t value)
{
    protocol::writeVarInt(out, value);
}
// Appends raw bytes to a byte buffer.
inline void bytes(Bytes &out, const std::uint8_t *data, std::size_t size)
{
    if (size)
    {
        out.insert(out.end(), data, data + size);
    }
}
// Appends a length-prefixed UTF-8 string to a byte buffer.
inline void string(Bytes &out, const std::string &value)
{
    var(out, static_cast<std::int32_t>(value.size()));
    bytes(out, reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
}
// Appends a big-endian unsigned 16-bit integer to a byte buffer.
inline void u16(Bytes &out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
// Appends a big-endian unsigned 64-bit integer to a byte buffer.
inline void u64(Bytes &out, std::uint64_t v)
{
    for (int i = 7; i >= 0; --i)
    {
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    }
}
// Appends a big-endian 32-bit float to a byte buffer.
inline void f32(Bytes &out, float v)
{
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    for (int i = 3; i >= 0; --i)
    {
        out.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    }
}
// Appends a big-endian 64-bit double to a byte buffer.
inline void f64(Bytes &out, double v)
{
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    u64(out, bits);
}
// Packs block coordinates into the protocol's positional long.
inline std::uint64_t blockPosition(int x, int y, int z)
{
    return ((std::uint64_t(x) & 0x3ffffffULL) << 38) | ((std::uint64_t(z) & 0x3ffffffULL) << 12) |
           (std::uint64_t(y) & 0xfffULL);
}
} // namespace craft::wire
