#include "protocol.hpp"

#include <algorithm>
#include <limits>
#include <zlib.h>

namespace craft::protocol
{
// Reads a bounded VarInt without advancing offset on incomplete/invalid input.
DecodeResult readVarInt(const std::uint8_t *data, std::size_t size, std::size_t &offset, std::int32_t &value) noexcept
{
    if (!data && size)
    {
        return DecodeResult::Invalid;
    }
    std::uint32_t result = 0;
    std::size_t at = offset;
    for (unsigned i = 0; i < 5; ++i)
    {
        if (at >= size)
        {
            return DecodeResult::NeedMore;
        }
        const std::uint8_t byte = data[at++];
        if (i == 4 && (byte & 0xf0u))
        {
            return DecodeResult::Invalid;
        }
        result |= std::uint32_t(byte & 0x7fu) << (7u * i);
        if (!(byte & 0x80u))
        {
            value = static_cast<std::int32_t>(result);
            offset = at;
            return DecodeResult::Packet;
        }
    }
    return DecodeResult::Invalid;
}

// Writes a VarInt into a byte buffer.
void writeVarInt(std::vector<std::uint8_t> &out, std::int32_t value)
{
    auto bits = static_cast<std::uint32_t>(value);
    do
    {
        std::uint8_t byte = static_cast<std::uint8_t>(bits & 0x7f);
        bits >>= 7;
        if (bits)
        {
            byte |= 0x80;
        }
        out.push_back(byte);
    } while (bits);
}

// Builds the complete uncompressed handshake frame.
bool handshake(const std::string &host, std::uint16_t port, int intent, std::vector<std::uint8_t> &frame)
{
    if (host.empty() || host.size() > 255 || (intent != 1 && intent != 2 && intent != 3))
    {
        return false;
    }
    std::vector<std::uint8_t> body;
    body.reserve(host.size() + 16);
    writeVarInt(body, 0);
    writeVarInt(body, Java263);
    writeVarInt(body, static_cast<std::int32_t>(host.size()));
    body.insert(body.end(), host.begin(), host.end());
    body.push_back(static_cast<std::uint8_t>(port >> 8));
    body.push_back(static_cast<std::uint8_t>(port));
    writeVarInt(body, intent);
    frame.clear();
    writeVarInt(frame, static_cast<std::int32_t>(body.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    return true;
}

// Appends newly received bytes to the input buffer.
bool Decoder::append(const std::uint8_t *data, std::size_t size)
{
    if (!data && size)
    {
        return false;
    }
    if (consumed_)
    {
        input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }
    // Five bytes leave room for the length prefix.
    if (size > MaxPacketBytes + 5 || input_.size() > MaxPacketBytes + 5 - size)
    {
        return false;
    }
    if (size)
    {
        input_.insert(input_.end(), data, data + size);
    }
    return true;
}

// Decodes the next complete packet from the input buffer.
DecodeResult Decoder::next(std::int32_t &id, const std::uint8_t *&payload, std::size_t &payloadSize)
{
    payload = nullptr;
    payloadSize = 0;
    std::size_t at = consumed_;
    std::int32_t length = 0;
    auto result = readVarInt(input_.data(), input_.size(), at, length);
    if (result != DecodeResult::Packet)
    {
        return result;
    }
    if (length <= 0)
    {
        return DecodeResult::Invalid;
    }
    if (static_cast<std::size_t>(length) > MaxPacketBytes)
    {
        return DecodeResult::TooLarge;
    }
    if (input_.size() - at < static_cast<std::size_t>(length))
    {
        return DecodeResult::NeedMore;
    }
    const auto end = at + static_cast<std::size_t>(length);
    const std::uint8_t *body = input_.data() + at;
    std::size_t bodySize = static_cast<std::size_t>(length);
    std::size_t bodyOffset = 0;
    if (threshold_ >= 0)
    {
        std::int32_t expandedSize = 0;
        result = readVarInt(body, bodySize, bodyOffset, expandedSize);
        if (result != DecodeResult::Packet || expandedSize < 0)
        {
            return DecodeResult::Invalid;
        }
        if (expandedSize)
        {
            if (expandedSize < threshold_)
            {
                return DecodeResult::Invalid;
            }
            if (static_cast<std::size_t>(expandedSize) > MaxPacketBytes)
            {
                return DecodeResult::TooLarge;
            }
            decoded_.resize(static_cast<std::size_t>(expandedSize));
            z_stream stream{};
            stream.next_in = const_cast<Bytef *>(body + bodyOffset);
            stream.avail_in = static_cast<uInt>(bodySize - bodyOffset);
            stream.next_out = decoded_.data();
            stream.avail_out = static_cast<uInt>(decoded_.size());
            if (inflateInit(&stream) != Z_OK)
            {
                return DecodeResult::CompressionError;
            }
            const int code = inflate(&stream, Z_FINISH);
            const bool valid = code == Z_STREAM_END && stream.avail_in == 0 && stream.avail_out == 0;
            inflateEnd(&stream);
            if (!valid)
            {
                return DecodeResult::CompressionError;
            }
            body = decoded_.data();
            bodySize = decoded_.size();
            bodyOffset = 0;
        }
        else if (bodySize - bodyOffset >= static_cast<std::size_t>(threshold_))
        {
            return DecodeResult::Invalid;
        }
    }
    result = readVarInt(body, bodySize, bodyOffset, id);
    if (result != DecodeResult::Packet || id < 0)
    {
        return DecodeResult::Invalid;
    }
    payload = body + bodyOffset;
    payloadSize = bodySize - bodyOffset;
    consumed_ = end;
    return DecodeResult::Packet;
}

// Releases all buffered data and resets compression state.
void Decoder::reset()
{
    std::vector<std::uint8_t>().swap(input_);
    std::vector<std::uint8_t>().swap(decoded_);
    consumed_ = 0;
    threshold_ = -1;
}
} // namespace craft::protocol
