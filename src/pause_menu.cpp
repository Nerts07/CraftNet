#include "pause_menu.hpp"
#include "screen_ui.hpp"
#include "session.hpp"
#include "text_draw.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>

namespace craft
{
namespace
{
constexpr int MouseOffset = 512;
constexpr int VisibleRows = 9;
constexpr Rectangle BackButton{132, 231, 216, 20};
constexpr bool usesHotbarModifier(Action action)
{
    return action == Action::Sprint || action == Action::Drop || action == Action::PreviousSlot ||
           action == Action::NextSlot;
}
struct Binding
{
    const char *name;
    int key, pad;
};
constexpr Binding Bindings[] = {
    {"Forward", KEY_W, 0}, {"Backward", KEY_S, 0}, {"Strafe left", KEY_A, 0}, {"Strafe right", KEY_D, 0},
    {"Look left", 0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT}, {"Look right", 0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
    {"Look up", 0, GAMEPAD_BUTTON_RIGHT_FACE_UP}, {"Look down", 0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
    {"Jump", KEY_SPACE, GAMEPAD_BUTTON_MIDDLE_LEFT}, {"Sprint", KEY_LEFT_CONTROL, GAMEPAD_BUTTON_MIDDLE_LEFT},
    {"Sneak", KEY_LEFT_SHIFT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT}, {"Chat", KEY_T, GAMEPAD_BUTTON_LEFT_FACE_UP},
    {"Inventory", KEY_E, GAMEPAD_BUTTON_LEFT_FACE_DOWN},
    {"Attack / mine", MouseOffset + MOUSE_BUTTON_LEFT, GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
    {"Use / place", MouseOffset + MOUSE_BUTTON_RIGHT, GAMEPAD_BUTTON_LEFT_TRIGGER_1},
    {"Drop item", KEY_Q, GAMEPAD_BUTTON_LEFT_FACE_DOWN}, {"Hotbar modifier", 0, GAMEPAD_BUTTON_LEFT_FACE_LEFT},
    {"Previous slot", 0, GAMEPAD_BUTTON_LEFT_TRIGGER_1}, {"Next slot", 0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
    {"Slot 1", KEY_ONE, 0}, {"Slot 2", KEY_TWO, 0}, {"Slot 3", KEY_THREE, 0},
    {"Slot 4", KEY_FOUR, 0}, {"Slot 5", KEY_FIVE, 0}, {"Slot 6", KEY_SIX, 0},
    {"Slot 7", KEY_SEVEN, 0}, {"Slot 8", KEY_EIGHT, 0}, {"Slot 9", KEY_NINE, 0},
};
static_assert(std::size(Bindings) == static_cast<std::size_t>(Action::Count));

#if defined(__PSP__)
constexpr int Distances[] = {16, 24, 32, 48};
constexpr int FrameRates[] = {20, 30, 60, 0};
#else
constexpr int Distances[] = {32, 48, 64, 80, 96, 128};
constexpr int FrameRates[] = {30, 60, 120, 144, 240, 0};
#endif

bool nativeBinding(const Binding &binding)
{
#if defined(__PSP__)
    return binding.pad != 0;
#else
    return binding.key != 0;
#endif
}

const std::vector<Action> &controlActions()
{
    static const auto actions = []
    {
        std::vector<Action> result;
        for (std::size_t i = 0; i < std::size(Bindings); ++i)
        {
            if (nativeBinding(Bindings[i]))
            {
                result.push_back(static_cast<Action>(i));
            }
        }
        return result;
    }();
    return actions;
}

bool padHit(int button)
{
    return IsGamepadAvailable(0) && IsGamepadButtonPressed(0, button);
}

const char *padName(int button)
{
    switch (button)
    {
    case GAMEPAD_BUTTON_LEFT_FACE_UP: return "D-pad Up";
    case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return "D-pad Down";
    case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return "D-pad Left";
    case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return "D-pad Right";
    case GAMEPAD_BUTTON_RIGHT_FACE_UP: return "Triangle";
    case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return "Cross";
    case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return "Square";
    case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "Circle";
    case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return "L";
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return "R";
    case GAMEPAD_BUTTON_MIDDLE_LEFT: return "Select";
    default: return nullptr;
    }
}

bool validKey(int code)
{
    return (code > KEY_NULL && code < MouseOffset && code != KEY_ESCAPE && code != KEY_F11) ||
           (code >= MouseOffset && code <= MouseOffset + MOUSE_BUTTON_BACK);
}

int captureBinding()
{
#if defined(__PSP__)
    for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_MIDDLE_RIGHT; ++button)
    {
        if (padName(button) && padHit(button))
        {
            return button;
        }
    }
#else
    if (const int key = GetKeyPressed(); validKey(key))
    {
        return key;
    }
    for (int button = MOUSE_BUTTON_LEFT; button <= MOUSE_BUTTON_BACK; ++button)
    {
        if (IsMouseButtonPressed(button))
        {
            return MouseOffset + button;
        }
    }
#endif
    return 0;
}

template <std::size_t N> void cycle(int &value, const int (&options)[N], int step)
{
    const auto index = std::find(std::begin(options), std::end(options), value) - options;
    value = options[(static_cast<int>(index) + step + N) % N];
}

void button(Font font, Rectangle rect, const std::string &text, bool selected, bool centered = true)
{
    DrawRectangleRec(rect, selected ? Color{96, 100, 124, 255} : Color{58, 60, 65, 255});
    DrawRectangleLinesEx(rect, 1, selected ? RAYWHITE : Color{146, 148, 152, 255});
    const float textWidth = gameTextWidth(font, text.c_str(), 10);
    const float size = !centered && textWidth > rect.width - 12 ? std::max(6.0f, 10 * (rect.width - 12) / textWidth) : 10;
    const float width = std::min(textWidth * size / 10, rect.width - 12);
    const float x = rect.x + (centered ? (rect.width - width) / 2 : 6);
    drawClippedText(font, text.c_str(), {x, rect.y + (rect.height - size) / 2}, size, rect.width - 12, RAYWHITE);
}
} // namespace

GameSettings::GameSettings()
{
    resetBindings();
}

void GameSettings::resetBindings()
{
    for (std::size_t i = 0; i < std::size(Bindings); ++i)
    {
        keys[i] = Bindings[i].key;
        pads[i] = Bindings[i].pad;
    }
}

void GameSettings::load()
{
    for (const char *path : {"settings.dat", "settings.dat.bak"})
    {
        std::ifstream file(path);
        int version, view, cap, showChat, showBoard, viewBob, speed;
        auto newKeys = keys;
        auto newPads = pads;
        if (!(file >> version >> view >> cap >> showChat >> showBoard >> viewBob >> speed) || version != 1)
        {
            continue;
        }
        bool valid = true;
        for (std::size_t i = 0; i < std::size(Bindings); ++i)
        {
            if (!(file >> newKeys[i] >> newPads[i]) || (Bindings[i].key ? !validKey(newKeys[i]) : newKeys[i] != 0) ||
                (Bindings[i].pad ? !padName(newPads[i]) : newPads[i] != 0))
            {
                valid = false;
                break;
            }
        }
        if (!valid)
        {
            continue;
        }
        if (std::find(std::begin(Distances), std::end(Distances), view) != std::end(Distances))
        {
            viewBlocks = view;
        }
        if (std::find(std::begin(FrameRates), std::end(FrameRates), cap) != std::end(FrameRates))
        {
            fps = cap;
        }
        chat = showChat != 0;
        scoreboard = showBoard != 0;
        bob = viewBob != 0;
        sensitivity = std::clamp(speed, 25, 250);
        keys = newKeys;
        pads = newPads;
        return;
    }
}

bool GameSettings::save() const
{
    std::ofstream file("settings.dat.tmp", std::ios::trunc);
    file << 1 << ' ' << viewBlocks << ' ' << fps << ' ' << chat << ' ' << scoreboard << ' ' << bob << ' '
         << sensitivity << '\n';
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        file << keys[i] << ' ' << pads[i] << '\n';
    }
    file.close();
    if (!file)
    {
        std::remove("settings.dat.tmp");
        return false;
    }
    std::ifstream existing("settings.dat");
    if (existing)
    {
        existing.close();
        std::remove("settings.dat.bak");
        if (std::rename("settings.dat", "settings.dat.bak") != 0)
        {
            std::remove("settings.dat.tmp");
            return false;
        }
    }
    if (std::rename("settings.dat.tmp", "settings.dat") != 0)
    {
        std::rename("settings.dat.bak", "settings.dat");
        std::remove("settings.dat.tmp");
        return false;
    }
    return true;
}

bool GameSettings::desktopHeld(Action action) const
{
    const int key = keys[static_cast<std::size_t>(action)];
    return key >= MouseOffset ? IsMouseButtonDown(key - MouseOffset) : key && IsKeyDown(key);
}

bool GameSettings::desktopPressed(Action action) const
{
    const int key = keys[static_cast<std::size_t>(action)];
    return key >= MouseOffset ? IsMouseButtonPressed(key - MouseOffset) : key && IsKeyPressed(key);
}

bool GameSettings::padHeld(Action action) const
{
    const int pad = pads[static_cast<std::size_t>(action)];
    return pad && IsGamepadAvailable(0) && IsGamepadButtonDown(0, pad);
}

bool GameSettings::padPressed(Action action) const
{
    const int pad = pads[static_cast<std::size_t>(action)];
    return pad && padHit(pad);
}

bool GameSettings::held(Action action) const { return desktopHeld(action) || padHeld(action); }
bool GameSettings::pressed(Action action) const { return desktopPressed(action) || padPressed(action); }

void GameSettings::bind(Action action, int code)
{
#if defined(__PSP__)
    pads[static_cast<std::size_t>(action)] = code;
#else
    keys[static_cast<std::size_t>(action)] = code;
#endif
}

std::string GameSettings::bindingName(Action action) const
{
#if defined(__PSP__)
    return padName(pads[static_cast<std::size_t>(action)]);
#else
    const int code = keys[static_cast<std::size_t>(action)];
    if (code >= MouseOffset)
    {
        return "Mouse " + std::to_string(code - MouseOffset + 1);
    }
    switch (code)
    {
    case KEY_LEFT_CONTROL: return "Left Ctrl";
    case KEY_RIGHT_CONTROL: return "Right Ctrl";
    case KEY_LEFT_SHIFT: return "Left Shift";
    case KEY_RIGHT_SHIFT: return "Right Shift";
    case KEY_LEFT_ALT: return "Left Alt";
    case KEY_RIGHT_ALT: return "Right Alt";
    case KEY_SPACE: return "Space";
    case KEY_ENTER: return "Enter";
    case KEY_TAB: return "Tab";
    case KEY_UP: return "Up";
    case KEY_DOWN: return "Down";
    case KEY_LEFT: return "Left";
    case KEY_RIGHT: return "Right";
    default:
        if (code >= KEY_F1 && code <= KEY_F12)
        {
            return "F" + std::to_string(code - KEY_F1 + 1);
        }
        if (const char *name = GetKeyName(code); name && *name)
        {
            return name;
        }
        return "Key " + std::to_string(code);
    }
#endif
}

void PauseMenu::show(Page page)
{
    page_ = page;
    selected_ = first_ = 0;
    capturing_ = false;
}

void PauseMenu::open()
{
    show(Page::Main);
    players_.clear();
    rosterRevision_ = 0;
    saveFailed_ = false;
}

void PauseMenu::changed(const GameSettings &settings)
{
    saveFailed_ = !settings.save();
}

int PauseMenu::rows() const
{
    switch (page_)
    {
    case Page::Main: return 5;
    case Page::Options: return 6;
    case Page::Controls: return static_cast<int>(controlActions().size()) + 1;
    case Page::Players: return std::max(1, static_cast<int>(players_.size()));
    default: return 0;
    }
}

Rectangle PauseMenu::rowRect(int row) const
{
    return {104, static_cast<float>((page_ == Page::Main ? 62 : 42) +
                                  (row - first_) * (page_ == Page::Main ? 29 : 20)),
            272, page_ == Page::Main ? 23.0f : 18.0f};
}

std::string PauseMenu::rowText(int row, const GameSettings &settings) const
{
    if (page_ == Page::Main)
    {
        constexpr const char *labels[] = {"Resume Game", "Players", "Video Settings", "Controls", "Disconnect"};
        return labels[row];
    }
    if (page_ == Page::Controls)
    {
        if (row == static_cast<int>(controlActions().size()))
        {
            return "Reset bindings";
        }
        const auto action = controlActions()[row];
        std::string binding = capturing_ && row == selected_ ? "Press a button..." : settings.bindingName(action);
#if defined(__PSP__)
        if (usesHotbarModifier(action) && !(capturing_ && row == selected_))
        {
            binding = settings.bindingName(Action::HotbarModifier) + " + " + binding;
        }
#endif
        return std::string(Bindings[static_cast<std::size_t>(action)].name) + "  " + binding;
    }
    if (page_ == Page::Players)
    {
        return players_.empty() ? "No players listed by the server" : players_[row];
    }
    const auto toggle = [](bool value) { return value ? "On" : "Off"; };
    switch (row)
    {
    case 0: return "Render distance  " + (settings.viewBlocks == 24 ? std::string("1.5") :
                                            std::to_string(settings.viewBlocks / 16)) + " chunks";
    case 1: return "FPS limit  " + (settings.fps ? std::to_string(settings.fps) : std::string("Unlimited"));
    case 2: return std::string("Show chat  ") + toggle(settings.chat);
    case 3: return std::string("Show scoreboard  ") + toggle(settings.scoreboard);
    case 4: return std::string("View bobbing  ") + toggle(settings.bob);
    default: return "Look sensitivity  " + std::to_string(settings.sensitivity) + "%";
    }
}

PauseMenu::Result PauseMenu::update(const Session &session, GameSettings &settings)
{
    const bool pause = IsKeyPressed(KEY_ESCAPE) || padHit(GAMEPAD_BUTTON_MIDDLE_RIGHT);
    if (capturing_)
    {
        if (pause)
        {
            capturing_ = false;
        }
        else if (const int code = captureBinding())
        {
            settings.bind(controlActions()[selected_], code);
            changed(settings);
            capturing_ = false;
        }
        return Result::Stay;
    }
    const auto mouse = GetScreenToWorld2D(GetMousePosition(), uiCamera());
    const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (pause || padHit(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
        (page_ != Page::Main && click && CheckCollisionPointRec(mouse, BackButton)))
    {
        if (page_ == Page::Main)
        {
            close();
            return Result::Resume;
        }
        show(Page::Main);
        return Result::Stay;
    }
    if (page_ == Page::Players && rosterRevision_ != session.rosterRevision())
    {
        rosterRevision_ = session.rosterRevision();
        players_ = session.tabPlayers();
    }
    const float wheel = GetMouseWheelMove();
    const bool up = IsKeyPressed(KEY_UP) || padHit(GAMEPAD_BUTTON_LEFT_FACE_UP) || wheel > 0;
    const bool down = IsKeyPressed(KEY_DOWN) || padHit(GAMEPAD_BUTTON_LEFT_FACE_DOWN) || wheel < 0;
    const bool paging = page_ == Page::Players || page_ == Page::Controls;
    const bool previous = paging && (IsKeyPressed(KEY_PAGE_UP) || padHit(GAMEPAD_BUTTON_LEFT_TRIGGER_1));
    const bool next = paging && (IsKeyPressed(KEY_PAGE_DOWN) || padHit(GAMEPAD_BUTTON_RIGHT_TRIGGER_1));
    const int step = static_cast<int>(down) - static_cast<int>(up) +
                     VisibleRows * (static_cast<int>(next) - static_cast<int>(previous));
    selected_ = std::clamp(selected_ + step, 0, rows() - 1);
    first_ = std::clamp(first_, std::max(0, selected_ - VisibleRows + 1),
                        std::min(selected_, std::max(0, rows() - VisibleRows)));
    bool clickedRow = false;
    for (int row = first_; click && row < std::min(rows(), first_ + VisibleRows); ++row)
    {
        if (CheckCollisionPointRec(mouse, rowRect(row)))
        {
            selected_ = row;
            clickedRow = true;
            break;
        }
    }
    const bool activate = clickedRow || IsKeyPressed(KEY_ENTER) || padHit(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    if (page_ == Page::Main && activate)
    {
        switch (selected_)
        {
        case 0: close(); return Result::Resume;
        case 1: players_ = session.tabPlayers(); rosterRevision_ = session.rosterRevision(); show(Page::Players); break;
        case 2: show(Page::Options); break;
        case 3: show(Page::Controls); break;
        case 4: close(); return Result::Disconnect;
        }
    }
    else if (page_ == Page::Options)
    {
        const bool left = IsKeyPressed(KEY_LEFT) || padHit(GAMEPAD_BUTTON_LEFT_FACE_LEFT) ||
                          (clickedRow && mouse.x < 240);
        const bool right = IsKeyPressed(KEY_RIGHT) || padHit(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || activate;
        if (left || right)
        {
            const int step = left ? -1 : 1;
            switch (selected_)
            {
            case 0: cycle(settings.viewBlocks, Distances, step); break;
            case 1: cycle(settings.fps, FrameRates, step); SetTargetFPS(settings.fps); break;
            case 2: settings.chat = !settings.chat; break;
            case 3: settings.scoreboard = !settings.scoreboard; break;
            case 4: settings.bob = !settings.bob; break;
            case 5: settings.sensitivity = std::clamp(settings.sensitivity + step * 5, 25, 250); break;
            }
            changed(settings);
        }
    }
    else if (page_ == Page::Controls && activate)
    {
        if (selected_ == static_cast<int>(controlActions().size()))
        {
            settings.resetBindings();
            changed(settings);
        }
        else
        {
            while (GetKeyPressed())
            {
            }
            capturing_ = true;
        }
    }
    return Result::Stay;
}

void PauseMenu::draw(Font font, const GameSettings &settings) const
{
    if (!active())
    {
        return;
    }
    const auto camera = uiCamera();
    DrawRectangleRec({-camera.offset.x / camera.zoom, -camera.offset.y / camera.zoom,
                      GetScreenWidth() / camera.zoom, GetScreenHeight() / camera.zoom}, Color{0, 0, 0, 160});
    const std::string title = page_ == Page::Main ? "Game Menu" : page_ == Page::Options ? "Video Settings" :
                              page_ == Page::Controls ? "Controls" : "Players (" + std::to_string(players_.size()) + ")";
    const float x = 240 - gameTextWidth(font, title.c_str(), 16) / 2;
    drawClippedText(font, title.c_str(), {x, 17}, 16, 460, RAYWHITE);
    for (int row = first_; row < std::min(rows(), first_ + VisibleRows); ++row)
    {
        const auto rect = rowRect(row);
        const bool player = page_ == Page::Players && !players_.empty();
        const auto text = rowText(row, settings);
        beginUiScissor(rect.x, rect.y, rect.width, rect.height);
        button(font, rect, text, row == selected_, !player);
        EndScissorMode();
    }
    if (page_ != Page::Main)
    {
        button(font, BackButton, "Back", false);
    }
    const char *hint = saveFailed_ ? "Settings could not be saved" :
                       capturing_ ? "Esc / Start cancels" :
                       rows() > VisibleRows ? "Up / Down to scroll   PgUp / PgDn or L / R to page" : "";
    drawClippedText(font, hint, {104, 256}, 8, 272, RAYWHITE);
}
} // namespace craft
