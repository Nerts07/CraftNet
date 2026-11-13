#pragma once
#include "inventory.hpp"
#include <raylib.h>
#include <string_view>
#include <vector>

namespace craft
{
class Session;
struct Sidebar;

class GameUi final
{
  public:
    ~GameUi();
    void load();
    void update(Session &session, bool justOpened, bool closeRequested);
    void drawHud(const Inventory &inventory, Font font) const;
    void drawSidebar(const Sidebar &sidebar, Font font, float right, float top) const;
    void drawInventory(const Inventory &inventory, Font font) const;
    void drawDrops(const Session &session, Camera3D camera) const;
    void drawHand(const ItemStack &item, Texture2D skin, float progress) const;
    Texture2D crackAtlas() const { return gui_; }
    void drawEffects(const Session &session, Font font) const;
    std::string_view itemName(int id) const;

  private:
    struct Icon
    {
        unsigned tile = 65535;
        std::string name;
    };
    void drawItem(const ItemStack &item, float x, float y, Font font) const;
    Texture2D sprite(int id, Rectangle &source) const;
    std::vector<Icon> icons_;
    std::array<Texture2D, 4> pages_{};
    Texture2D gui_{};
    unsigned revision_ = 0;
    int selected_ = -1, direction_ = -1;
    double nextRepeat_ = 0;
};
} // namespace craft
