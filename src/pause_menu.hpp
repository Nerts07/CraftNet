#pragma once
#include <raylib.h>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace craft
{
class Session;

enum class Action
{
    Forward, Backward, StrafeLeft, StrafeRight,
    LookLeft, LookRight, LookUp, LookDown,
    Jump, Sprint, Sneak, Chat, Inventory, Attack, Use, Drop,
    HotbarModifier, PreviousSlot, NextSlot,
    Slot1, Slot2, Slot3, Slot4, Slot5, Slot6, Slot7, Slot8, Slot9,
    Count
};

struct GameSettings
{
    GameSettings();
    void load();
    bool save() const;
    void resetBindings();
    bool held(Action action) const;
    bool pressed(Action action) const;
    bool padHeld(Action action) const;
    bool padPressed(Action action) const;
    bool desktopHeld(Action action) const;
    bool desktopPressed(Action action) const;
    void bind(Action action, int code);
    std::string bindingName(Action action) const;

#if defined(__PSP__)
    int viewBlocks = 32, fps = 30;
#else
    int viewBlocks = 80, fps = 0;
#endif
    bool chat = true, scoreboard = true, bob = true;
    int sensitivity = 100;

  private:
    static constexpr std::size_t ActionCount = static_cast<std::size_t>(Action::Count);
    std::array<int, ActionCount> keys{}, pads{};
};

class PauseMenu
{
  public:
    enum class Result { Stay, Resume, Disconnect };
    void open();
    void close() { page_ = Page::Closed; }
    bool active() const { return page_ != Page::Closed; }
    Result update(const Session &session, GameSettings &settings);
    void draw(Font font, const GameSettings &settings) const;

  private:
    enum class Page { Closed, Main, Players, Options, Controls };
    void show(Page page);
    int rows() const;
    Rectangle rowRect(int row) const;
    std::string rowText(int row, const GameSettings &settings) const;
    void changed(const GameSettings &settings);

    Page page_ = Page::Closed;
    int selected_ = 0, first_ = 0;
    bool capturing_ = false, saveFailed_ = false;
    unsigned rosterRevision_ = 0;
    std::vector<std::string> players_;
};
} // namespace craft
