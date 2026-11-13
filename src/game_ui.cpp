#include "game_ui.hpp"
#include "screen_ui.hpp"
#include "session.hpp"
#include "text_draw.hpp"
#include <raymath.h>
#include <rlgl.h>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace craft
{
namespace
{
struct Slot
{
    Vector2 position;
    const ItemStack *item;
};

struct Layout
{
    float x = 152, y = 0;
    int height = 166, count = 0;
    std::array<Slot, 90> slots{};
};

Layout layout(const Inventory &inv)
{
    Layout result;
    result.height = inv.rows > 0 ? 114 + inv.rows * 18 : 166;
    result.y = (272 - result.height) / 2.0f - 6;
    auto &slots = result.slots;
    const auto add = [&](int x, int y, const ItemStack &item)
    {
        slots[result.count++] = {{result.x + x, result.y + y}, &item};
    };
    if (inv.rows < 0)
    {
        return result;
    }
    if (inv.container == 0)
    {
        add(154, 28, inv.player[0]);
        for (int i = 0; i < 4; ++i)
        {
            add(98 + (i % 2) * 18, 18 + (i / 2) * 18, inv.player[1 + i]);
        }
        for (int i = 0; i < 4; ++i)
        {
            add(8, 8 + i * 18, inv.player[5 + i]);
        }
    }
    else
    {
        for (int i = 0; i < inv.rows * 9; ++i)
        {
            add(8 + (i % 9) * 18, 18 + (i / 9) * 18, inv.chest[i]);
        }
    }
    const int playerY = inv.container == 0 ? 84 : 32 + inv.rows * 18;
    for (int i = 0; i < 36; ++i)
    {
        add(8 + (i % 9) * 18, playerY + (i / 9) * 18 + (i >= 27 ? 4 : 0), inv.player[9 + i]);
    }
    if (inv.container == 0)
    {
        add(77, 62, inv.player[45]);
    }
    return result;
}

bool down(int button)
{
    return IsGamepadAvailable(0) && IsGamepadButtonDown(0, button);
}

bool pressed(int button)
{
    return IsGamepadAvailable(0) && IsGamepadButtonPressed(0, button);
}

Texture2D loadTexture(const char *path, int width, int height)
{
    if (!FileExists(path))
    {
        return {};
    }
    Image image = LoadImage(path);
    Texture2D result{};
    if (image.data && image.width == width && image.height == height)
    {
#if defined(__PSP__)
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R4G4B4A4);
#endif
        result = LoadTextureFromImage(image);
        SetTextureFilter(result, TEXTURE_FILTER_POINT);
    }
    UnloadImage(image);
    return result;
}

void centered(Font font, const std::string &text, float y, float size, Color color)
{
    const float width = std::min(456.0f, gameTextWidth(font, text.c_str(), size));
    drawClippedText(font, text.c_str(), {240 - width / 2, y}, size, width + 1, color);
}
} // namespace

GameUi::~GameUi()
{
    if (gui_.id)
    {
        UnloadTexture(gui_);
    }
    for (const auto page : pages_)
    {
        if (page.id)
        {
            UnloadTexture(page);
        }
    }
}

void GameUi::load()
{
    gui_ = loadTexture("assets/imported/compiled/inventory-gui.png", 256, 512);
    if (FILE *file = std::fopen("assets/imported/compiled/inventory-items.bin", "rb"))
    {
        std::uint16_t count;
        if (std::fread(&count, sizeof(count), 1, file) == 1 && count <= 4096)
        {
            icons_.resize(4096);
            for (unsigned i = 0; i < count; ++i)
            {
                std::uint16_t id, tile;
                const bool header = std::fread(&id, 2, 1, file) == 1 && std::fread(&tile, 2, 1, file) == 1;
                const int length = header ? std::fgetc(file) : EOF;
                char name[128];
                if (length < 0 || length >= 128 || id >= icons_.size() ||
                    std::fread(name, 1, static_cast<std::size_t>(length), file) != static_cast<std::size_t>(length))
                {
                    break;
                }
                auto &icon = icons_[id];
                icon.tile = tile;
                icon.name.assign(name, static_cast<std::size_t>(length));
                std::replace(icon.name.begin(), icon.name.end(), '_', ' ');
            }
        }
        std::fclose(file);
    }
    for (unsigned page = 0; page < pages_.size(); ++page)
    {
        char path[64];
        std::snprintf(path, sizeof(path), "assets/imported/compiled/inventory-items%u.png", page);
        pages_[page] = loadTexture(path, 512, 512);
    }
}

void GameUi::update(Session &session, bool justOpened, bool closeRequested)
{
    const auto &inv = session.inventory();
    const auto menu = layout(inv);
    const int direction = IsKeyDown(KEY_UP) || down(GAMEPAD_BUTTON_LEFT_FACE_UP) ? 0
                        : IsKeyDown(KEY_DOWN) || down(GAMEPAD_BUTTON_LEFT_FACE_DOWN) ? 1
                        : IsKeyDown(KEY_LEFT) || down(GAMEPAD_BUTTON_LEFT_FACE_LEFT) ? 2
                        : IsKeyDown(KEY_RIGHT) || down(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ? 3 : -1;
    if (justOpened || revision_ != inv.revision)
    {
        revision_ = inv.revision;
        selected_ = menu.count ? (inv.container == 0 ? 9 : 0) : -1;
        direction_ = direction;
        nextRepeat_ = GetTime() + 0.35;
        if (justOpened)
        {
            return;
        }
    }
    if (closeRequested || pressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
    {
        session.closeInventory();
        return;
    }
    const double now = GetTime();
    if (selected_ >= 0 && direction >= 0 && (direction != direction_ || now >= nextRepeat_))
    {
        nextRepeat_ = now + (direction == direction_ ? 0.09 : 0.35);
        const auto origin = menu.slots[selected_].position;
        float best = std::numeric_limits<float>::max();
        int candidate = selected_;
        for (int i = 0; i < menu.count; ++i)
        {
            const auto p = menu.slots[i].position;
            const float advance = direction == 0 ? origin.y - p.y : direction == 1 ? p.y - origin.y
                                  : direction == 2 ? origin.x - p.x : p.x - origin.x;
            const float across = direction < 2 ? std::abs(p.x - origin.x) : std::abs(p.y - origin.y);
            const float score = advance + across * 4;
            if (advance > 0 && score < best)
            {
                best = score;
                candidate = i;
            }
        }
        selected_ = candidate;
    }
    direction_ = direction;
    const bool left = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool right = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    const auto mouse = GetMouseDelta();
    if (left || right || mouse.x || mouse.y)
    {
        const auto cursor = GetScreenToWorld2D(GetMousePosition(), uiCamera());
        for (int i = 0; i < menu.count; ++i)
        {
            const auto p = menu.slots[i].position;
            if (CheckCollisionPointRec(cursor, {p.x - 1, p.y - 1, 18, 18}))
            {
                selected_ = i;
                if (left || right)
                {
                    session.clickInventory(i, right ? 1 : 0);
                    return;
                }
                break;
            }
        }
    }
    if (selected_ >= 0 && (IsKeyPressed(KEY_ENTER) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)))
    {
        session.clickInventory(selected_, 0);
    }
}

std::string_view GameUi::itemName(int id) const
{
    return id > 0 && static_cast<std::size_t>(id) < icons_.size() ? std::string_view(icons_[id].name) :
                                                                  std::string_view{};
}

Texture2D GameUi::sprite(int id, Rectangle &source) const
{
    const unsigned tile = itemName(id).empty() ? 65535 : icons_[id].tile;
    const unsigned page = tile / 1024;
    if (page >= pages_.size() || !pages_[page].id)
    {
        source = {80, 428, 16, 16};
        return id > 0 ? gui_ : Texture2D{};
    }
    source = {static_cast<float>((tile % 32) * 16), static_cast<float>((tile / 32 % 32) * 16), 16, 16};
    return pages_[page];
}

void GameUi::drawItem(const ItemStack &item, float x, float y, Font font) const
{
    if (!item.count)
    {
        return;
    }
    Rectangle source;
    const auto texture = sprite(item.id, source);
    if (texture.id)
    {
        DrawTextureRec(texture, source, {x, y}, WHITE);
    }
    else
    {
        drawClippedText(font, "?", {x + 5, y + 2}, 12, 12, LIGHTGRAY);
    }
    if (item.count > 1)
    {
        char text[12];
        std::snprintf(text, sizeof(text), "%d", item.count);
        const float left = x + 16 - gameTextWidth(font, text, 8);
        drawClippedText(font, text, {left + 1, y + 9}, 8, 32, BLACK);
        drawClippedText(font, text, {left, y + 8}, 8, 32, WHITE);
    }
}

void GameUi::drawDrops(const Session &session, Camera3D camera) const
{
    struct Visible { const RemotePlayer *drop; float distance; };
    std::array<Visible, 64> visible{};
    std::size_t count = 0;
    for (const auto &drop : session.drops())
    {
        if (!drop.active || !drop.itemCount || drop.invisible)
        {
            continue;
        }
        const Vector3 pos{static_cast<float>(drop.x), static_cast<float>(drop.y), static_cast<float>(drop.z)};
        const float distance = Vector3DistanceSqr(pos, camera.position);
        if (distance < 24 * 24)
        {
            visible[count++] = {&drop, distance};
        }
    }
    std::sort(visible.begin(), visible.begin() + count, [](Visible a, Visible b) { return a.distance > b.distance; });
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    const float time = static_cast<float>(GetTime());
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &drop = *visible[i].drop;
        Rectangle source;
        const auto texture = sprite(drop.equipment[0], source);
        if (!texture.id)
        {
            continue;
        }
        const float phase = time * 2.2f + (drop.id % 31) * 0.37f;
        const float x = static_cast<float>(drop.x), y = static_cast<float>(drop.y) + 0.25f + std::sin(phase) * 0.08f,
                    z = static_cast<float>(drop.z);
        constexpr float radius = 0.2f;
        const float dx = std::cos(phase) * radius, dz = std::sin(phase) * radius;
        const Vector3 vertices[] = {{x - dx, y + radius, z - dz}, {x - dx, y - radius, z - dz},
                                    {x + dx, y - radius, z + dz}, {x + dx, y + radius, z + dz}};
        const float u0 = source.x / texture.width, u1 = (source.x + source.width) / texture.width;
        const float v0 = source.y / texture.height, v1 = (source.y + source.height) / texture.height;
        const Vector2 uv[] = {{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}};
        rlSetTexture(texture.id);
        rlBegin(RL_TRIANGLES);
        rlColor4ub(255, 255, 255, 255);
        for (const unsigned index : {0u, 1u, 2u, 0u, 2u, 3u})
        {
            rlTexCoord2f(uv[index].x, uv[index].y);
            rlVertex3f(vertices[index].x, vertices[index].y, vertices[index].z);
        }
        rlEnd();
        rlSetTexture(0);
    }
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

void GameUi::drawSidebar(const Sidebar &sidebar, Font font, float right, float top) const
{
    if (!sidebar.visible)
    {
        return;
    }
    constexpr float size = 9, lineHeight = 10, padding = 4, maxWidth = 164;
    const float titleWidth = gameTextWidth(font, sidebar.title.c_str(), size);
    float contentWidth = titleWidth;
    for (int i = 0; i < sidebar.count; ++i)
    {
        const auto &row = sidebar.rows[i];
        const float value = row.value.empty() ? 0 : gameTextWidth(font, row.value.c_str(), size) + padding;
        contentWidth = std::max(contentWidth, gameTextWidth(font, row.label.c_str(), size) + value);
    }
    contentWidth = std::clamp(contentWidth, 24.0f, maxWidth - 2 * padding);
    const float width = contentWidth + 2 * padding;
    const float height = (sidebar.count + 1) * lineHeight + 4;
    const float left = right - width;
    float y = top;
    DrawRectangleRec({left, y, width, height}, Color{0, 0, 0, 116});
    DrawRectangleRec({left, y, width, lineHeight + 2}, Color{0, 0, 0, 45});
    beginUiScissor(left, y, width, height);
    y += 2;
    drawClippedText(font, sidebar.title.c_str(), {left + (width - std::min(titleWidth, contentWidth)) / 2, y},
                    size, contentWidth, RAYWHITE);
    for (int i = 0; i < sidebar.count; ++i)
    {
        y += lineHeight;
        const auto &row = sidebar.rows[i];
        const float valueWidth = std::min(contentWidth, gameTextWidth(font, row.value.c_str(), size));
        const float labelWidth = contentWidth - (valueWidth > 0 ? valueWidth + padding : 0);
        drawClippedText(font, row.label.c_str(), {left + padding, y}, size, labelWidth, RAYWHITE);
        drawClippedText(font, row.value.c_str(), {right - padding - valueWidth, y}, size, valueWidth, RAYWHITE);
    }
    EndScissorMode();
}

void GameUi::drawHand(const ItemStack &item, Texture2D skin, float progress) const
{
    const float swing = std::sin(progress * PI);
    const float reach = std::sin(std::sqrt(progress) * PI);
    const float rotation = -25 - swing * 42;
    const Vector2 grip{408 - 73 * reach, 214 - 40 * swing};
    if (skin.id)
    {
        const Rectangle arm{grip.x, grip.y, 44, 112};
        // Skin UVs start at the shoulder. The fist sits at the top of this quad.
        DrawTexturePro(skin, {44, 20, 4, -12}, arm, {22, 22}, rotation, WHITE);
        DrawTexturePro(skin, {44, 36, 4, -12}, arm, {22, 22}, rotation, WHITE);
    }
    Rectangle source;
    const auto texture = sprite(item.id, source);
    if (item.count && texture.id)
    {
        const auto name = itemName(item.id);
        const auto endsWith = [name](std::string_view suffix)
        {
            return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
        };
        const bool tool = endsWith("axe") || endsWith("sword") || endsWith("shovel") || endsWith("hoe") ||
                          endsWith("mace") || endsWith("rod") || endsWith("trident") || endsWith("spear");
        DrawTexturePro(texture, source, {grip.x, grip.y, 92, 92}, tool ? Vector2{17, 78} : Vector2{48, 69},
                       -22 - swing * 49, WHITE);
    }
}

void GameUi::drawEffects(const Session &session, Font font) const
{
    const float alpha = session.titleAlpha();
    if (session.dead())
    {
        DrawRectangle(0, 0, 480, 272, {100, 0, 0, 120});
        centered(font, "You died!", 66, 24, WHITE);
        centered(font, session.deathMessage(), 102, 11, LIGHTGRAY);
        DrawRectangle(151, 152, 178, 24, {43, 40, 44, 235});
        DrawRectangleLines(151, 152, 178, 24, LIGHTGRAY);
        centered(font, session.respawnRequested() ? "Waiting for server..." : "Enter / Cross to respawn", 158, 11,
                 WHITE);
    }
    else if (session.spectator() && alpha <= 0 && !session.actionBarVisible())
    {
        centered(font, "Spectator", 226, 12, LIGHTGRAY);
    }
    if (alpha > 0)
    {
        const Color tint{255, 255, 255, static_cast<unsigned char>(alpha * 255)};
        centered(font, session.title(), 86, 30, tint);
        centered(font, session.subtitle(), 122, 15, tint);
    }
    if (session.actionBarVisible())
    {
        centered(font, session.actionBar(), 215, 13, WHITE);
    }
}

void GameUi::drawHud(const Inventory &inv, Font font) const
{
    constexpr int x = 149, y = 250;
    if (gui_.id)
    {
        DrawTextureRec(gui_, {0, 388, 182, 22}, {x, y}, WHITE);
        DrawTextureRec(gui_, {184, 388, 24, 23}, {x - 1.0f + inv.selected * 20, y - 1}, WHITE);
        DrawTextureRec(gui_, {0, 414, 182, 5}, {x, y - 7}, WHITE);
        DrawTextureRec(gui_, {0, 420, std::floor(inv.experience * 182), 5}, {x, y - 7}, WHITE);
        const int hearts = std::clamp(static_cast<int>(std::ceil(inv.maxHealth / 2)), 1, 40);
        const int health = static_cast<int>(std::ceil(std::min(inv.health, inv.maxHealth)));
        for (int i = 0; i < hearts; ++i)
        {
            const Vector2 heart{x + 1.0f + (i % 10) * 8, y - 17.0f - (i / 10) * 9};
            DrawTextureRec(gui_, {0, 428, 9, 9}, heart, WHITE);
            const int amount = std::clamp(health - i * 2, 0, 2);
            if (amount)
            {
                DrawTextureRec(gui_, {amount * 10.0f, 428, 9, 9}, heart, WHITE);
            }
        }
        for (int i = 0; i < 10; ++i)
        {
            const int food = std::clamp(inv.food - i * 2, 0, 2);
            DrawTextureRec(gui_, {30.0f + food * 10, 428, 9, 9}, {x + 172.0f - i * 8, y - 17}, WHITE);
        }
    }
    else
    {
        DrawRectangle(x, y, 182, 22, DARKGRAY);
        DrawRectangleLines(x + inv.selected * 20, y, 22, 22, WHITE);
    }
    for (int i = 0; i < 9; ++i)
    {
        drawItem(inv.hotbar(i), x + 3.0f + i * 20, y + 3, font);
    }
    if (inv.level > 0)
    {
        centered(font, std::to_string(inv.level), y - 19, 10, GREEN);
    }
}

void GameUi::drawInventory(const Inventory &inv, Font font) const
{
    const auto menu = layout(inv);
    DrawRectangle(0, 0, 480, 272, {0, 0, 0, 155});
    if (gui_.id && inv.rows >= 0)
    {
        if (inv.container == 0)
        {
            DrawTextureRec(gui_, {0, 0, 176, 166}, {menu.x, menu.y}, WHITE);
        }
        else
        {
            const float chestHeight = 17.0f + inv.rows * 18;
            DrawTextureRec(gui_, {0, 166, 176, chestHeight}, {menu.x, menu.y}, WHITE);
            DrawTextureRec(gui_, {0, 291, 176, 97}, {menu.x, menu.y + chestHeight}, WHITE);
        }
    }
    else
    {
        DrawRectangle(static_cast<int>(menu.x), static_cast<int>(menu.y), 176, menu.height, LIGHTGRAY);
    }
    if (inv.container != 0)
    {
        drawClippedText(font, inv.title.empty() ? "Container" : inv.title.c_str(), {menu.x + 8, menu.y + 5}, 10, 160,
                        DARKGRAY);
        const float labelY = menu.y + (inv.rows > 0 ? 21 + inv.rows * 18 : 32);
        drawClippedText(font, inv.rows < 0 ? "Unsupported container" : "Inventory", {menu.x + 8, labelY}, 10, 160,
                        DARKGRAY);
    }
    else
    {
        drawClippedText(font, "Crafting", {menu.x + 96, menu.y + 5}, 10, 78, DARKGRAY);
        drawClippedText(font, "Inventory", {menu.x + 8, menu.y + 72}, 10, 160, DARKGRAY);
    }
    for (int i = 0; i < menu.count; ++i)
    {
        const auto &slot = menu.slots[i];
        const auto p = slot.position;
        if (!gui_.id || i == selected_)
        {
            DrawRectangle(static_cast<int>(p.x), static_cast<int>(p.y), 16, 16, {255, 255, 255, 95});
        }
        drawItem(*slot.item, p.x, p.y, font);
        if (i == selected_)
        {
            DrawRectangleLines(static_cast<int>(p.x) - 1, static_cast<int>(p.y) - 1, 18, 18, {255, 245, 160, 255});
        }
    }
    if (selected_ >= 0 && selected_ < menu.count)
    {
        const auto &slot = menu.slots[selected_];
        if (inv.carried.count)
        {
            drawItem(inv.carried, slot.position.x + 9, slot.position.y + 8, font);
        }
        else if (slot.item->count)
        {
            const auto &item = *slot.item;
            constexpr float fontSize = 10, lineHeight = 11, maxTextWidth = 406;
            struct Line { std::string text; Color color; };
            std::vector<Line> lines;
            const auto add = [&](const std::string &text, Color color)
            {
                if (lines.size() >= 21)
                {
                    return;
                }
                std::size_t start = 0;
                do
                {
                    const auto end = text.find('\n', start);
                    std::string line = text.substr(start, end == std::string::npos ? end : end - start);
                    while (gameTextWidth(font, line.c_str(), fontSize) > maxTextWidth && lines.size() < 21)
                    {
                        std::size_t split = line.rfind(' ');
                        while (split != std::string::npos && split > 0 &&
                               gameTextWidth(font, line.substr(0, split).c_str(), fontSize) > maxTextWidth)
                        {
                            split = line.rfind(' ', split - 1);
                        }
                        if (split == std::string::npos || split == 0)
                        {
                            break;
                        }
                        lines.push_back({line.substr(0, split), color});
                        line.erase(0, split + 1);
                    }
                    if (lines.size() < 21)
                    {
                        lines.push_back({std::move(line), color});
                    }
                    if (end == std::string::npos)
                    {
                        break;
                    }
                    start = end + 1;
                } while (lines.size() < 21);
            };
            std::string name(itemName(item.id));
            add(!item.name.empty() ? item.name : name.empty() ? "Item" : name, WHITE);
            constexpr const char *enchantments[] = {
#include "item_enchantments.inc"
            };
            constexpr const char *levels[] = {"0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
            for (const auto &entry : item.enchantments)
            {
                const auto index = static_cast<std::size_t>(entry[0]);
                std::string label = index < std::size(enchantments) ? enchantments[index] :
                                                                      "Enchantment " + std::to_string(index);
                label += ' ';
                label += entry[1] <= 10 ? levels[entry[1]] : std::to_string(entry[1]);
                add(label, {170, 170, 170, 255});
            }
            for (const auto &line : item.lore)
            {
                add(line, {170, 170, 170, 255});
            }
            if (item.maxDamage > 0)
            {
                add("Durability " + std::to_string(std::max(0, item.maxDamage - item.damage)) + " / " +
                    std::to_string(item.maxDamage), GRAY);
            }
            else if (item.damage > 0)
            {
                add("Damage " + std::to_string(item.damage), GRAY);
            }
            if (!name.empty())
            {
                std::replace(name.begin(), name.end(), ' ', '_');
                add("minecraft:" + name, GRAY);
            }
            if (item.components)
            {
                add(std::to_string(item.components) + " component changes", GRAY);
            }
            float width = 0;
            for (const auto &line : lines)
            {
                width = std::max(width, gameTextWidth(font, line.text.c_str(), fontSize));
            }
            width = std::min(width, maxTextWidth) + 10;
            const float height = lines.size() * lineHeight + 8;
            const float x = std::clamp(slot.position.x + 12, 4.0f, 476.0f - width);
            const float y = std::clamp(slot.position.y + 17, 4.0f, 268.0f - height);
            DrawRectangleRec({x, y, width, height}, {21, 8, 29, 245});
            DrawRectangleLinesEx({x, y, width, height}, 1, {92, 33, 138, 255});
            float lineY = y + 4;
            for (const auto &line : lines)
            {
                drawClippedText(font, line.text.c_str(), {x + 5, lineY}, fontSize, width - 10, line.color);
                lineY += lineHeight;
            }
        }
    }
}
} // namespace craft
