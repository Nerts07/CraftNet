#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace craft::protocol
{
constexpr std::int32_t Java263 = 777;
constexpr std::size_t MaxPacketBytes = 2u * 1024u * 1024u;

enum class State
{
    Handshake,
    Status,
    Login,
    Configuration,
    Play,
    Disconnected
};

enum class DecodeResult
{
    Packet,
    NeedMore,
    Invalid,
    TooLarge,
    CompressionError
};

// Reads a bounded VarInt without advancing offset on incomplete/invalid input.
DecodeResult readVarInt(const std::uint8_t *data, std::size_t size, std::size_t &offset, std::int32_t &value) noexcept;
// Writes a VarInt into a byte buffer.
void writeVarInt(std::vector<std::uint8_t> &out, std::int32_t value);

// Builds the handshake: status (1), login (2), or server-directed transfer (3).
bool handshake(const std::string &host, std::uint16_t port, int intent, std::vector<std::uint8_t> &frame);

// One connection owns one decoder. A returned payload stays valid only until
// the next next(), reset(), or destruction. Call next() until NeedMore before
// appending more data; malformed input must terminate the connection.
class Decoder final
{
  public:
    // Appends newly received bytes to the input buffer.
    bool append(const std::uint8_t *data, std::size_t size);
    // Decodes the next complete packet from the input buffer.
    DecodeResult next(std::int32_t &id, const std::uint8_t *&payload, std::size_t &payloadSize);
    // Enables or disables compressed packet decoding.
    void setCompression(std::int32_t threshold) noexcept
    {
        threshold_ = threshold;
    }
    // Releases all buffered data and resets compression state.
    void reset();
    // Returns the total number of bytes currently allocated.
    std::size_t allocatedBytes() const noexcept
    {
        return input_.capacity() + decoded_.capacity();
    }

  private:
    std::vector<std::uint8_t> input_;
    std::vector<std::uint8_t> decoded_;
    std::size_t consumed_ = 0;
    std::int32_t threshold_ = -1;
};
} // namespace craft::protocol
