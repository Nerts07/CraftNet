#include "world.hpp"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

namespace craft
{
namespace
{
// Converts a world coordinate to its section coordinate.
std::int32_t sectionOf(std::int32_t v) noexcept
{
    return v / 16 - ((v % 16) < 0 ? 1 : 0);
}
// Converts a world coordinate to its in-section local coordinate.
std::size_t localOf(std::int32_t v) noexcept
{
    int r = v % 16;
    return static_cast<std::size_t>(r < 0 ? r + 16 : r);
}
// Computes the block index inside a section.
std::size_t indexOf(std::int32_t x, std::int32_t y, std::int32_t z) noexcept
{
    return (localOf(y) * 16 + localOf(z)) * 16 + localOf(x);
}
// Computes the section coordinate containing a world position.
SectionPos posOf(std::int32_t x, std::int32_t y, std::int32_t z) noexcept
{
    return {sectionOf(x), sectionOf(y), sectionOf(z)};
}
// Returns the squared distance between two section positions.
std::uint64_t dist(SectionPos a, SectionPos b)
{
    // Coordinates can span all int32; cap distance to avoid overflow.
    auto axis = [](std::int32_t v, std::int32_t c)
    {
        auto d = std::int64_t(v) - c;
        return static_cast<std::uint64_t>(std::min<std::int64_t>(d < 0 ? -d : d, 1000000));
    };
    const auto x = axis(a.x, b.x), y = axis(a.y, b.y), z = axis(a.z, b.z);
    return x * x + y * y + z * z;
}
} // namespace

// Computes the starting hash slot for a section position.
std::size_t World::slot(SectionPos p) const noexcept
{
    auto h = std::uint32_t(p.x) * 0x8da6b343u ^ std::uint32_t(p.y) * 0xd8163841u ^ std::uint32_t(p.z) * 0xcb1ab31fu;
    return h & (TableSize - 1);
}

// Finds a cached section, if present.
World::Entry *World::find(SectionPos p) noexcept
{
    return const_cast<Entry *>(static_cast<const World *>(this)->find(p));
}

// Finds a cached section, if present (const overload).
const World::Entry *World::find(SectionPos p) const noexcept
{
    auto i = slot(p);
    for (std::size_t n = 0; n < TableSize; ++n, i = (i + 1) & (TableSize - 1))
    {
        const auto &e = sections_[i];
        if (!e.used)
        {
            return nullptr;
        }
        if (e.used == 1 && e.pos == p)
        {
            return &e;
        }
    }
    return nullptr;
}

// Evicts the farthest section to make room.
bool World::evict(SectionPos incoming, bool allocatedOnly)
{
    Entry *farthest = nullptr;
    std::uint64_t farDistance = 0;
    const SectionPos center = hasServerCenter_ ? SectionPos{serverCenter_.x, center_.y, serverCenter_.z} : center_;
    for (auto &e : sections_)
    {
        if (e.used == 1 && (!allocatedOnly || e.blocks))
        {
            auto d = dist(e.pos, center);
            if (!farthest || d > farDistance)
            {
                farthest = &e;
                farDistance = d;
            }
        }
    }
    if (!farthest || dist(incoming, center) > farDistance)
    {
        return false;
    }
    if (farthest->blocks)
    {
        farthest->blocks.reset();
        --allocated_;
    }
    const auto pos = farthest->pos;
    farthest->used = 2;
    --count_;
    ++tombstones_;
    dirtyAround(pos);
    return true;
}

// Rehashes the table when tombstones accumulate.
void World::compact() noexcept
{
    // Rehash occasionally so travelling through many chunks does not turn all
    // table slots into tombstones and make every lookup scan the whole table.
    if (tombstones_ < TableSize / 4)
    {
        return;
    }
    std::unique_ptr<std::array<Entry, TableSize>> fresh(new (std::nothrow) std::array<Entry, TableSize>{});
    if (!fresh)
    {
        return;
    }
    for (auto &e : sections_)
    {
        if (e.used == 1)
        {
            auto i = slot(e.pos);
            while ((*fresh)[i].used == 1)
            {
                i = (i + 1) & (TableSize - 1);
            }
            (*fresh)[i] = std::move(e);
        }
    }
    sections_.swap(*fresh);
    tombstones_ = 0;
}

// Bumps revision numbers around a changed section.
void World::dirtyAround(SectionPos p, unsigned sides) noexcept
{
    ++revision_;
    const SectionPos offsets[] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (unsigned i = 0; i < 7; ++i)
    {
        if (i && !(sides & (1u << (i - 1))))
        {
            continue;
        }
        const auto d = offsets[i];
        const auto x = std::int64_t(p.x) + d.x, y = std::int64_t(p.y) + d.y, z = std::int64_t(p.z) + d.z;
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX || z < INT32_MIN || z > INT32_MAX)
        {
            continue;
        }
        if (auto *e = find({static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)}))
        {
            e->version = revision_;
        }
    }
}

// Stores a full section of blocks, possibly as a uniform section.
bool World::put(SectionPos p, const BlockState *blocks, std::size_t count)
{
    if (!blocks || count != BlocksPerSection)
    {
        return false;
    }
    const bool uniform = std::all_of(blocks + 1, blocks + count,
                                     [&](BlockState v)
                                     {
                                         return v == blocks[0];
                                     });
    return store(p, blocks, blocks[0], uniform);
}

bool World::putUniform(SectionPos p, BlockState state)
{
    return store(p, nullptr, state, true);
}

bool World::section(SectionPos p, BlockState &uniform, const Blocks *&blocks, std::uint64_t &version) const
{
    const auto *entry = find(p);
    if (!entry)
    {
        return false;
    }
    uniform = entry->uniform;
    blocks = entry->blocks.get();
    version = entry->version;
    return true;
}

bool World::store(SectionPos p, const BlockState *blocks, BlockState state, bool uniform)
{
    compact();
    Entry *e = find(p);
    if (e && uniform && !e->blocks && e->uniform == state)
    {
        return true;
    }
    if (e && !uniform && e->blocks && std::equal(blocks, blocks + BlocksPerSection, e->blocks->begin()))
    {
        return true;
    }
    if (!uniform && (!e || !e->blocks) && allocated_ >= MaxAllocatedSections && !evict(p, true))
    {
        return false;
    }
    if (!e && count_ >= MaxSections && !evict(p, false))
    {
        return false;
    }
    // evict() may have evicted the target itself.
    e = find(p);
    if (!e)
    {
        auto i = slot(p);
        std::size_t target = TableSize;
        for (std::size_t n = 0; n < TableSize; ++n, i = (i + 1) & (TableSize - 1))
        {
            if (sections_[i].used == 2 && target == TableSize)
            {
                target = i;
            }
            if (!sections_[i].used)
            {
                if (target == TableSize)
                {
                    target = i;
                }
                break;
            }
        }
        if (target == TableSize)
        {
            return false;
        }
        e = &sections_[target];
        if (e->used == 2)
        {
            --tombstones_;
        }
        e->pos = p;
        e->used = 1;
        ++count_;
    }
    if (uniform)
    {
        if (e->blocks)
        {
            e->blocks.reset();
            --allocated_;
        }
        e->uniform = state;
    }
    else
    {
        if (!e->blocks)
        {
            std::unique_ptr<Blocks> allocation(new (std::nothrow) Blocks);
            if (!allocation)
            {
                e->used = 2;
                --count_;
                ++tombstones_;
                return false;
            }
            e->blocks = std::move(allocation);
            ++allocated_;
            peakBytes_ = std::max(peakBytes_, allocatedBytes());
        }
        std::copy_n(blocks, BlocksPerSection, e->blocks->begin());
    }
    dirtyAround(p);
    return true;
}

// Reads one block state by world coordinate.
bool World::get(std::int32_t x, std::int32_t y, std::int32_t z, BlockState &out) const
{
    auto *e = find(posOf(x, y, z));
    if (!e)
    {
        return false;
    }
    out = e->blocks ? (*e->blocks)[indexOf(x, y, z)] : e->uniform;
    return true;
}

// Writes one block state by world coordinate.
bool World::set(std::int32_t x, std::int32_t y, std::int32_t z, BlockState state)
{
    auto p = posOf(x, y, z);
    auto *e = find(p);
    if (!e)
    {
        return false;
    }
    if (!e->blocks)
    {
        if (e->uniform == state)
        {
            return true;
        }
        const auto old = e->uniform;
        if (allocated_ >= MaxAllocatedSections && !evict(p, true))
        {
            return false;
        }
        e = find(p);
        if (!e)
        {
            return false;
        }
        std::unique_ptr<Blocks> allocation(new (std::nothrow) Blocks);
        if (!allocation)
        {
            return false;
        }
        allocation->fill(old);
        e->blocks = std::move(allocation);
        ++allocated_;
        peakBytes_ = std::max(peakBytes_, allocatedBytes());
    }
    const auto index = indexOf(x, y, z);
    auto &current = (*e->blocks)[index];
    if (current != state)
    {
        current = state;
        // An interior block cannot change any neighbouring section's boundary
        // faces. Rebuild neighbours only when the changed block touches them.
        unsigned sides = 0;
        sides |= (index & 15) == 15 ? 1u : 0u;
        sides |= (index & 15) == 0 ? 2u : 0u;
        sides |= (index >> 8) == 15 ? 4u : 0u;
        sides |= (index >> 8) == 0 ? 8u : 0u;
        sides |= ((index >> 4) & 15) == 15 ? 16u : 0u;
        sides |= ((index >> 4) & 15) == 0 ? 32u : 0u;
        dirtyAround(p, sides);
    }
    return true;
}

// Removes one section from the cache.
void World::erase(SectionPos p)
{
    auto *e = find(p);
    if (!e)
    {
        return;
    }
    if (e->blocks)
    {
        e->blocks.reset();
        --allocated_;
    }
    e->used = 2;
    --count_;
    ++tombstones_;
    dirtyAround(p);
}

// Removes every cached section in a world column.
void World::eraseColumn(std::int32_t x, std::int32_t z)
{
    for (auto &e : sections_)
    {
        if (e.used == 1 && e.pos.x == x && e.pos.z == z)
        {
            auto pos = e.pos;
            if (e.blocks)
            {
                e.blocks.reset();
                --allocated_;
            }
            e.used = 2;
            --count_;
            ++tombstones_;
            dirtyAround(pos);
        }
    }
}

// Drops cached sections outside the requested radius.
void World::retainAround(SectionPos center, int horizontalRadius, int verticalRadius)
{
    for (auto &e : sections_)
    {
        if (e.used == 1 && (std::abs(std::int64_t(e.pos.x) - center.x) > horizontalRadius ||
                            std::abs(std::int64_t(e.pos.z) - center.z) > horizontalRadius ||
                            std::abs(std::int64_t(e.pos.y) - center.y) > verticalRadius))
        {
            const auto pos = e.pos;
            if (e.blocks)
            {
                e.blocks.reset();
                --allocated_;
            }
            e.used = 2;
            --count_;
            ++tombstones_;
            dirtyAround(pos);
        }
    }
}

// Sets the world position used for eviction priority.
void World::setCenter(std::int32_t x, std::int32_t y, std::int32_t z)
{
    center_ = posOf(x, y, z);
}

void World::setServerCenter(std::int32_t x, std::int32_t z) noexcept
{
    serverCenter_ = {x, 0, z};
    hasServerCenter_ = true;
}

// Removes all cached sections.
void World::clear() noexcept
{
    for (auto &e : sections_)
    {
        e.blocks.reset();
        e.used = 0;
    }
    count_ = allocated_ = tombstones_ = 0;
    center_ = {};
    hasServerCenter_ = false;
    ++revision_;
}
} // namespace craft
