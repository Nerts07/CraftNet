#pragma once
#include <array>
#include <string>
#include <vector>

namespace craft
{
struct ItemStack
{
    int id = 0, count = 0;
    std::string name;
    std::vector<std::string> lore;
    std::vector<std::array<int, 2>> enchantments;
    int damage = 0, maxDamage = 0, components = 0;
    int dye = -1;
};

namespace wire { class Reader; }
bool readItemStack(wire::Reader &reader, ItemStack &item);

struct Inventory
{
    static constexpr int HotbarStart = 36;
    std::array<ItemStack, 46> player{};
    std::array<ItemStack, 54> chest{};
    ItemStack carried;
    std::string title;
    int container = 0, containerState = 0, playerState = 0, rows = 0;
    int selected = 0, food = 20, level = 0;
    float health = 20, maxHealth = 20, experience = 0;
    bool open = false;
    bool containerReady = false;
    unsigned revision = 0;

    const ItemStack &hotbar(int slot) const { return player[HotbarStart + slot]; }
};
} // namespace craft
