#include "chunk.hpp"
#include "wire.hpp"
#include <array>
#include <algorithm>

namespace craft
{
namespace
{
struct Palette
{
    unsigned bits = 0;
    std::array<BlockState, 256> values{};
    std::size_t count = 0;
    const std::uint8_t *data = nullptr;
    std::size_t words = 0;
};

// Reads one paletted container header and its packed word data.
bool palette(wire::Reader &reader, int entries, int indirectMax, int directMax, Palette &out)
{
    std::uint8_t bits = 0;
    if (!reader.byte(bits) || bits > directMax)
    {
        return false;
    }
    out.bits = bits;
    if (!bits)
    {
        std::int32_t v;
        if (!reader.var(v) || v < 0 || v > 65535)
        {
            return false;
        }
        out.count = 1;
        out.values[0] = static_cast<BlockState>(v);
    }
    else if (bits <= indirectMax)
    {
        std::int32_t n;
        if (!reader.var(n) || n <= 0 || n > 256 || n > (1 << bits))
        {
            return false;
        }
        out.count = static_cast<std::size_t>(n);
        for (std::int32_t i = 0; i < n; ++i)
        {
            std::int32_t v;
            if (!reader.var(v) || v < 0 || v > 65535)
            {
                return false;
            }
            out.values[static_cast<std::size_t>(i)] = static_cast<BlockState>(v);
        }
    }
    if (bits)
    {
        auto perWord = 64u / bits;
        out.words = (static_cast<unsigned>(entries) + perWord - 1) / perWord;
        out.data = reader.data();
        if (!reader.skip(out.words * 8))
        {
            return false;
        }
    }
    return true;
}

// Expands a palette into the full 16x16x16 block array.
bool unpack(const Palette &p, World::Blocks &out)
{
    if (!p.bits)
    {
        out.fill(p.values[0]);
        return true;
    }
    const auto perWord = 64u / p.bits;
    const std::uint64_t mask = (std::uint64_t(1) << p.bits) - 1;
    std::size_t at = 0;
    for (std::size_t i = 0; i < p.words; ++i)
    {
        std::uint64_t word = 0;
        const auto *bytes = p.data + i * 8;
        for (int j = 0; j < 8; ++j)
        {
            word = (word << 8) | bytes[j];
        }
        const auto count = std::min<std::size_t>(perWord, out.size() - at);
        for (std::size_t j = 0; j < count; ++j)
        {
            const auto value = static_cast<std::size_t>(word & mask);
            if (p.count && value >= p.count)
            {
                return false;
            }
            out[at++] = p.count ? p.values[value] : static_cast<BlockState>(value);
            word >>= p.bits;
        }
    }
    return true;
}
} // namespace

// Decodes one chunk packet and stores its sections in the world.
bool ChunkDecoder::decode(World &world, const std::uint8_t *data, std::size_t size, std::int32_t minY,
                          std::int32_t sections)
{
    if (sections < 1 || sections > 64 || minY < -2048 || minY > 2048)
    {
        return false;
    }
    wire::Reader r(data, size);
    std::int32_t x, z, heightmaps;
    if (!r.i32(x) || !r.i32(z) || x < -1875001 || x > 1875001 || z < -1875001 || z > 1875001 || !r.var(heightmaps) ||
        heightmaps < 0 || heightmaps > 16)
    {
        return false;
    }
    for (int i = 0; i < heightmaps; ++i)
    {
        std::int32_t kind, words;
        if (!r.var(kind) || !r.var(words) || kind < 0 || words < 0 || words > 64 ||
            !r.skip(static_cast<std::size_t>(words) * 8))
        {
            return false;
        }
    }
    std::int32_t length;
    if (!r.var(length) || length < 0 || static_cast<std::size_t>(length) > r.remaining())
    {
        return false;
    }
    wire::Reader chunks(r.data(), static_cast<std::size_t>(length));
    for (int y = 0; y < sections; ++y)
    {
        std::uint16_t nonAir, fluids;
        Palette blocks, biomes;
        if (!chunks.u16(nonAir) || !chunks.u16(fluids) || nonAir > 4096 || fluids > 4096 ||
            !palette(chunks, 4096, 8, 16, blocks) || (blocks.bits > 0 && blocks.bits < 4) ||
            !palette(chunks, 64, 3, 7, biomes))
        {
            return false;
        }
        // Capacity pressure deliberately drops the most distant sections.
        if (!blocks.bits)
        {
            world.putUniform({x, minY + y, z}, blocks.values[0]);
        }
        else
        {
            if (!unpack(blocks, scratch_))
            {
                return false;
            }
            world.put({x, minY + y, z}, scratch_.data(), scratch_.size());
        }
    }
    return chunks.remaining() == 0;
}
} // namespace craft
