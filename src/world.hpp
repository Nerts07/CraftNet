#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace craft
{
// Protocol 777's direct palette has 16 bits; the compiled registry has <=65536 states.
using BlockState = std::uint16_t;

struct SectionPos
{
    std::int32_t x, y, z;
    // Compares section coordinates.
    bool operator==(const SectionPos &o) const noexcept
    {
        return x == o.x && y == o.y && z == o.z;
    }
};

class World final
{
  public:
    static constexpr std::size_t Edge = 16;
    static constexpr std::size_t BlocksPerSection = 4096;
#if defined(__PSP__)
    static constexpr std::size_t MaxSections = 4096;
    static constexpr std::size_t MaxAllocatedSections = 1024;
#else
    static constexpr std::size_t MaxSections = 16384;
    static constexpr std::size_t MaxAllocatedSections = 8192;
#endif
    using Blocks = std::array<BlockState, BlocksPerSection>;

    // Stores a full section of blocks, possibly as a uniform section.
    bool put(SectionPos pos, const BlockState *blocks, std::size_t count);
    bool putUniform(SectionPos pos, BlockState state);
    bool section(SectionPos pos, BlockState &uniform, const Blocks *&blocks, std::uint64_t &version) const;
    // Reads one block state by world coordinate.
    bool get(std::int32_t x, std::int32_t y, std::int32_t z, BlockState &out) const;
    // Returns whether the section is currently cached.
    bool hasSection(SectionPos pos) const noexcept
    {
        return find(pos) != nullptr;
    }
    // Writes one block state by world coordinate.
    bool set(std::int32_t x, std::int32_t y, std::int32_t z, BlockState state);
    // Removes one section from the cache.
    void erase(SectionPos pos);
    // Removes every cached section in a world column.
    void eraseColumn(std::int32_t x, std::int32_t z);
    // Drops cached sections outside the requested radius.
    void retainAround(SectionPos center, int horizontalRadius, int verticalRadius);
    // Sets the world position used for eviction priority.
    void setCenter(std::int32_t x, std::int32_t y, std::int32_t z);
    void setServerCenter(std::int32_t x, std::int32_t z) noexcept;
    // Removes all cached sections.
    void clear() noexcept;
    // Returns the number of cached sections.
    std::size_t sectionCount() const noexcept
    {
        return count_;
    }
    // Returns the number of bytes allocated for block storage.
    std::size_t allocatedBytes() const noexcept
    {
        return allocated_ * sizeof(Blocks);
    }
    // Returns the peak number of block-storage bytes used.
    std::size_t peakBytes() const noexcept
    {
        return peakBytes_;
    }
    // Returns the revision that changes whenever cached sections change.
    std::uint64_t revision() const noexcept
    {
        return revision_;
    }
    // Visits every cached section.
    template <class Fn> void forEach(Fn &&fn) const
    {
        for (const auto &e : sections_)
        {
            if (e.used == 1)
            {
                fn(e.pos, e.uniform, e.blocks.get(), e.version);
            }
        }
    }

  private:
    bool store(SectionPos pos, const BlockState *blocks, BlockState state, bool uniform);
    // Open addressing keeps block lookups during meshing near O(1).
    struct Entry
    {
        SectionPos pos{};
        std::unique_ptr<Blocks> blocks;
        BlockState uniform = 0;
        std::uint64_t version = 0;
        std::uint8_t used = 0;
    };
    static constexpr std::size_t TableSize = MaxSections * 2;
    std::array<Entry, TableSize> sections_{};
    std::size_t count_ = 0, allocated_ = 0, peakBytes_ = 0, tombstones_ = 0;
    std::uint64_t revision_ = 1;
    SectionPos center_{};
    SectionPos serverCenter_{};
    bool hasServerCenter_ = false;
    // Computes the starting hash slot for a section position.
    std::size_t slot(SectionPos pos) const noexcept;
    // Finds a cached section, if present.
    Entry *find(SectionPos pos) noexcept;
    // Finds a cached section, if present (const overload).
    const Entry *find(SectionPos pos) const noexcept;
    // Evicts the farthest section to make room.
    bool evict(SectionPos incoming, bool allocatedOnly);
    // Bumps revision numbers around a changed section.
    void dirtyAround(SectionPos pos, unsigned sides = 63) noexcept;
    // Rehashes the table when tombstones accumulate.
    void compact() noexcept;
};
} // namespace craft
