#pragma once
#include "world.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace craft
{
// Decoder for Java 26.3 paletted chunk sections. Lighting and block entities
// are not rendered yet. The server-provided dimension determines the Y range.
class ChunkDecoder
{
  public:
    // Decodes one chunk packet and stores its sections in the world.
    bool decode(World &world, const std::uint8_t *data, std::size_t size, std::int32_t minSectionY,
                std::int32_t sectionCount);

  private:
    World::Blocks scratch_{};
};
} // namespace craft
