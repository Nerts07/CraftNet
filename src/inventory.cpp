#include "session.hpp"
#include "chat.hpp"
#include "dimension_nbt.hpp"
#include <algorithm>
#include <cmath>

namespace craft
{
namespace
{
struct ItemReader
{
    wire::Reader &r;

    bool var()
    {
        std::int32_t value;
        return r.var(value);
    }
    bool string()
    {
        std::int32_t length;
        return r.var(length) && length >= 0 && r.skip(static_cast<std::size_t>(length));
    }
    bool nbt()
    {
        std::uint8_t tag;
        int minY = 0, height = 0;
        unsigned found = 0;
        return r.byte(tag) && (!tag || dimensionValue(r, tag, 0, minY, height, found));
    }
    template <class F> bool optional(F read)
    {
        std::uint8_t present;
        return r.byte(present) && (!present || read());
    }
    template <class L, class R> bool either(L left, R right)
    {
        std::uint8_t first;
        return r.byte(first) && (first ? left() : right());
    }
    template <class F> bool holder(F direct)
    {
        std::int32_t id;
        return r.var(id) && id >= 0 && (id != 0 || direct());
    }
    template <class F> bool dispatch(F read)
    {
        std::int32_t id;
        return r.var(id) && read(id);
    }
    template <class F> bool repeat(int count, F read)
    {
        for (int i = 0; i < count; ++i)
        {
            if (!read())
            {
                return false;
            }
        }
        return true;
    }
    template <class F> bool list(F read, int maximum)
    {
        std::int32_t count;
        return r.var(count) && count >= 0 && count <= maximum && repeat(count, read);
    }
    bool holderSet()
    {
        std::int32_t size;
        return r.var(size) && size >= 0 && size <= 4097 &&
               (size == 0 ? string() : repeat(size - 1, [&] { return var(); }));
    }
    bool effectDetails(int depth)
    {
        return depth <= 16 && var() && var() && r.skip(3) && optional([&] { return effectDetails(depth + 1); });
    }
    bool typed(ItemStack *item, int depth)
    {
        std::int32_t id;
        if (depth > 16 || !r.var(id))
        {
            return false;
        }
        if (item && (id == 6 || id == 9))
        {
            std::string name;
            if (!readRichChatText(r, name))
            {
                return false;
            }
            if (id == 6 || item->name.empty())
            {
                item->name = std::move(name);
            }
            return true;
        }
        if (item && id == 11)
        {
            return list([&]
            {
                std::string line;
                if (!readRichChatText(r, line))
                {
                    return false;
                }
                if (item->lore.size() < 24)
                {
                    item->lore.push_back(std::move(line));
                }
                return true;
            }, 256);
        }
        if (item && (id == 13 || id == 45))
        {
            return list([&]
            {
                std::int32_t enchantment, level;
                if (!r.var(enchantment) || !r.var(level) || enchantment < 0 || level < 0)
                {
                    return false;
                }
                if (item->enchantments.size() < 24)
                {
                    item->enchantments.push_back({static_cast<int>(enchantment), static_cast<int>(level)});
                }
                return true;
            }, 256);
        }
        if (item && (id == 2 || id == 3))
        {
            std::int32_t value;
            if (!r.var(value) || value < 0)
            {
                return false;
            }
            (id == 2 ? item->maxDamage : item->damage) = value;
            return true;
        }
        if (item && id == 47)
        {
            std::int32_t rgb;
            if (!r.i32(rgb))
            {
                return false;
            }
            item->dye = rgb & 0xffffff;
            return true;
        }
        switch (id)
        {
#include "item_components.inc"
        default:
            return false;
        }
    }
    bool patch(ItemStack *item, int depth)
    {
        std::int32_t added, removed;
        if (depth > 16 || !r.var(added) || !r.var(removed) || added < 0 || added > 256 || removed < 0 || removed > 256)
        {
            return false;
        }
        if (item)
        {
            item->components = added + removed;
        }
        return repeat(added, [&] { return typed(item, depth + 1); }) && repeat(removed, [&] { return var(); });
    }
    bool stack(ItemStack &item)
    {
        std::int32_t count, id = 0;
        if (!r.var(count) || count < 0 || count > 999 || (count && (!r.var(id) || id <= 0)))
        {
            return false;
        }
        item.count = count;
        item.id = id;
        return !count || patch(&item, 0);
    }
};

ItemStack *slotAt(Inventory &inventory, int container, int slot)
{
    if (slot < 0)
    {
        return nullptr;
    }
    if (container == 0)
    {
        return slot < static_cast<int>(inventory.player.size()) ? &inventory.player[slot] : nullptr;
    }
    if (container != inventory.container || inventory.rows <= 0)
    {
        return nullptr;
    }
    const int slots = inventory.rows * 9;
    if (slot < slots)
    {
        return &inventory.chest[slot];
    }
    return slot < slots + 36 ? &inventory.player[9 + slot - slots] : nullptr;
}
} // namespace

bool readItemStack(wire::Reader &r, ItemStack &item)
{
    return ItemReader{r}.stack(item);
}

bool Session::readInventory(int id, wire::Reader &r)
{
    auto &inv = inventory_;
    std::int32_t container, state, value;
    if (id == 60)
    {
        std::string title;
        if (!r.var(container) || container <= 0 || !r.var(value) || value < 0 || !readRichChatText(r, title))
        {
            return false;
        }
        inv.container = container;
        inv.containerState = 0;
        inv.rows = value < 6 ? value + 1 : value == 20 ? 3 : -1;
        inv.title = std::move(title);
        inv.chest = {};
        inv.carried = {};
        inv.open = true;
        inv.containerReady = false;
        ++inv.revision;
        return true;
    }
    if (id == 17)
    {
        if (!r.var(container))
        {
            return false;
        }
        if (inv.container == container)
        {
            inv.open = false;
            inv.container = inv.rows = 0;
            inv.carried = {};
        }
        return true;
    }
    if (id == 106)
    {
        float health, saturation;
        if (!r.f32(health) || !r.var(value) || !r.f32(saturation) || !std::isfinite(health))
        {
            return false;
        }
        inv.health = std::max(0.0f, health);
        inv.food = std::clamp<std::int32_t>(value, 0, 20);
        if (health <= 0)
        {
            setDead();
        }
        else
        {
            dead_ = respawnRequested_ = false;
            deathMessage_.clear();
        }
        return true;
    }
    if (id == 105)
    {
        float progress;
        std::int32_t total;
        if (!r.f32(progress) || !r.var(value) || !r.var(total) || !std::isfinite(progress))
        {
            return false;
        }
        inv.experience = std::clamp(progress, 0.0f, 1.0f);
        inv.level = std::max<std::int32_t>(0, value);
        return true;
    }
    if (id == 107)
    {
        if (!r.var(value) || value < 0 || value >= 9)
        {
            return false;
        }
        inv.selected = value;
        return true;
    }
    ItemReader reader{r};
    if (id == 98 || id == 110)
    {
        ItemStack item;
        if ((id == 110 && !r.var(value)) || !reader.stack(item) || r.remaining())
        {
            return false;
        }
        if (id == 98)
        {
            inv.carried = std::move(item);
        }
        else if (value >= 0 && value < 36)
        {
            inv.player[value < 9 ? Inventory::HotbarStart + value : value] = std::move(item);
        }
        return true;
    }
    if (!r.var(container) || !r.var(state))
    {
        return false;
    }
    if (container != 0 && (container != inv.container || inv.rows <= 0))
    {
        return true;
    }
    if (id == 18)
    {
        std::array<ItemStack, 90> items{};
        ItemStack carried;
        if (!r.var(value) || value < 0 || value > static_cast<int>(items.size()))
        {
            return false;
        }
        for (int i = 0; i < value; ++i)
        {
            if (!reader.stack(items[i]))
            {
                return false;
            }
        }
        if (!reader.stack(carried) || r.remaining())
        {
            return false;
        }
        for (int i = 0, count = container == 0 ? 46 : inv.rows * 9 + 36; i < count; ++i)
        {
            *slotAt(inv, container, i) = std::move(items[i]);
        }
        inv.carried = std::move(carried);
    }
    else
    {
        std::uint16_t slot;
        ItemStack item;
        if (!r.u16(slot) || !reader.stack(item) || r.remaining())
        {
            return false;
        }
        if (auto *dest = slotAt(inv, container, static_cast<std::int16_t>(slot)))
        {
            *dest = std::move(item);
        }
    }
    (container == 0 ? inv.playerState : inv.containerState) = state;
    if (container != 0)
    {
        inv.containerReady = true;
    }
    return true;
}

void Session::openInventory()
{
    if (state_ == State::Play && !inventory_.open)
    {
        inventory_.open = true;
        ++inventory_.revision;
    }
}

void Session::closeInventory()
{
    if (state_ == State::Play && inventory_.open)
    {
        wire::Bytes body;
        wire::var(body, inventory_.container);
        sendPacket(19, body);
        inventory_.open = false;
        inventory_.container = inventory_.rows = 0;
        inventory_.carried = {};
    }
}

void Session::clickInventory(int slot, int button)
{
    const auto &inv = inventory_;
    if (state_ != State::Play || !inv.open || slot < 0 || slot >= (inv.container == 0 ? 46 : inv.rows * 9 + 36) ||
        (inv.container != 0 && (inv.rows < 1 || !inv.containerReady)))
    {
        return;
    }
    wire::Bytes body;
    wire::var(body, inv.container);
    // Without predicted hashes the server needs to send a full snapshot, including an empty cursor.
    const int state = inv.container == 0 ? inv.playerState : inv.containerState;
    wire::var(body, state ^ 1);
    wire::u16(body, static_cast<std::uint16_t>(slot));
    body.push_back(static_cast<std::uint8_t>(button));
    wire::var(body, 0); // PICKUP
    wire::var(body, 0); // No predicted slot hashes
    body.push_back(0);  // No predicted cursor hash
    sendPacket(18, body);
}

void Session::selectHotbar(int slot)
{
    if (state_ == State::Play && slot >= 0 && slot < 9 && slot != inventory_.selected)
    {
        wire::Bytes body;
        wire::u16(body, static_cast<std::uint16_t>(slot));
        if (sendPacket(54, body))
        {
            inventory_.selected = slot;
        }
    }
}

void Session::useItem()
{
    if (state_ == State::Play)
    {
        wire::Bytes body;
        wire::var(body, 0);
        wire::var(body, sequence_++);
        wire::f32(body, player_.yaw);
        wire::f32(body, player_.pitch);
        if (sendPacket(67, body))
        {
            swingHand();
        }
    }
}
} // namespace craft
