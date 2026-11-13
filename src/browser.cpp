#include "browser.hpp"
#include "offline_uuid.hpp"
#include "text.hpp"
#include "text_draw.hpp"
#include "screen_ui.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace craft
{
namespace
{
bool pressed(int button)
{
    return IsGamepadAvailable(0) && IsGamepadButtonPressed(0, button);
}
bool down(int button)
{
    return IsGamepadAvailable(0) && IsGamepadButtonDown(0, button);
}
bool menuHeld()
{
    constexpr int buttons[] = {GAMEPAD_BUTTON_LEFT_FACE_UP,    GAMEPAD_BUTTON_LEFT_FACE_DOWN,
                               GAMEPAD_BUTTON_LEFT_FACE_LEFT,  GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
                               GAMEPAD_BUTTON_RIGHT_FACE_UP,   GAMEPAD_BUTTON_RIGHT_FACE_DOWN,
                               GAMEPAD_BUTTON_RIGHT_FACE_LEFT, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
                               GAMEPAD_BUTTON_MIDDLE_LEFT,     GAMEPAD_BUTTON_MIDDLE_RIGHT};
    for (int button : buttons)
    {
        if (down(button))
        {
            return true;
        }
    }
    constexpr int keys[] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_ESCAPE, KEY_TAB};
    for (int key : keys)
    {
        if (IsKeyDown(key))
        {
            return true;
        }
    }
    return false;
}
bool click(Rectangle rect)
{
    return IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
           CheckCollisionPointRec(GetScreenToWorld2D(GetMousePosition(), uiCamera()), rect);
}
void label(Font font, const char *value, int x, int y, int width, Color color = RAYWHITE, int size = 14)
{
    beginUiScissor(x, y, width, size + 2);
    drawClippedText(font, value, {static_cast<float>(x), static_cast<float>(y)}, static_cast<float>(size),
                    static_cast<float>(width), color);
    EndScissorMode();
}
void label(Font font, const std::string &value, int x, int y, int width, Color color = RAYWHITE, int size = 14)
{
    label(font, value.c_str(), x, y, width, color, size);
}
void button(Font font, const char *title, Rectangle rect, bool enabled = true, bool focused = false)
{
    DrawRectangleRec(rect, enabled ? Color{49, 73, 63, 255} : Color{35, 42, 43, 255});
    if (focused)
    {
        DrawRectangleLinesEx(rect, 2, YELLOW);
    }
    label(font, title, static_cast<int>(rect.x + 5), static_cast<int>(rect.y + 5), static_cast<int>(rect.width - 8),
          enabled ? RAYWHITE : GRAY, 13);
}
void uiText(Font font, const char *text, int x, int y, int size, Color color)
{
    drawClippedText(font, text, {static_cast<float>(x), static_cast<float>(y)}, static_cast<float>(size),
                    static_cast<float>(480 - x), color);
}

void field(Font font, const char *title, const std::string &value, int y, bool focus)
{
    uiText(font, title, 18, y + 5, 13, LIGHTGRAY);
    DrawRectangle(124, y, 338, 28, focus ? Color{65, 92, 77, 255} : Color{37, 48, 47, 255});
    label(font, value, 131, y + 6, 324);
}
void editText(std::string &value, int maximum, bool ascii, bool digits)
{
    int code;
    while ((code = GetCharPressed()) > 0)
    {
        if (code < 32 || code == 127 || (ascii && code > 126) || (digits && (code < '0' || code > '9')))
        {
            continue;
        }
        const auto size = value.size();
        if (size < static_cast<std::size_t>(maximum))
        {
            appendUtf8(value, static_cast<unsigned>(code));
            if (value.size() > static_cast<std::size_t>(maximum))
            {
                value.resize(size);
            }
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
    {
        popUtf8(value);
    }
}
} // namespace

int ServerBrowser::navigation()
{
    const int direction = IsKeyDown(KEY_UP) || down(GAMEPAD_BUTTON_LEFT_FACE_UP)         ? 0
                          : IsKeyDown(KEY_DOWN) || down(GAMEPAD_BUTTON_LEFT_FACE_DOWN)   ? 1
                          : IsKeyDown(KEY_LEFT) || down(GAMEPAD_BUTTON_LEFT_FACE_LEFT)   ? 2
                          : IsKeyDown(KEY_RIGHT) || down(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ? 3
                                                                                         : -1;
    const double now = GetTime();
    if (direction < 0)
    {
        heldDirection_ = -1;
        return -1;
    }
    if (direction != heldDirection_)
    {
        heldDirection_ = direction;
        repeatAt_ = now + 0.35;
        return direction;
    }
    if (now >= repeatAt_)
    {
        repeatAt_ = now + 0.1;
        return direction;
    }
    return -1;
}

void ServerBrowser::start()
{
    servers_.load();
    if (!network_.beginNetwork())
    {
        notice_ = network_.error();
    }
}
void ServerBrowser::refresh()
{
    servers_.cancel();
    notice_.clear();
    refreshPending_ = true;
    if (network_.state() == Transport::State::Failed || network_.state() == Transport::State::Idle)
    {
        if (!network_.beginNetwork())
        {
            notice_ = network_.error();
        }
    }
}
void ServerBrowser::edit(bool adding)
{
    servers_.cancel();
    editing_ = adding ? servers_.size() : selected_;
    if (editing_ >= ServerList::Capacity)
    {
        notice_ = "Server list is full (24 entries)";
        return;
    }
    host_ = adding ? "" : servers_[editing_].host;
    port_ = adding ? "25565" : std::to_string(servers_[editing_].port);
    alias_ = adding ? "" : servers_[editing_].alias;
    focus_ = 0;
    notice_.clear();
    mode_ = Mode::Edit;
    while (GetCharPressed() > 0)
    {
    }
}
bool ServerBrowser::update(Keyboard &keyboard, std::string &externalNotice)
{
    if (keyboard.completed && keyboardTarget_ >= 0)
    {
        inputBlocked_ = true;
        keyboard.completed = false;
        if (keyboard.accepted)
        {
            switch (keyboardTarget_)
            {
            case 0:
                host_ = displayText(keyboard.result, 255);
                break;
            case 1:
                port_ = keyboard.result.substr(0, 5);
                break;
            case 2:
                alias_ = displayText(keyboard.result, 96);
                break;
            case 3:
                player_ = keyboard.result.substr(0, 16);
                break;
            }
        }
        keyboardTarget_ = -1;
        return false;
    }
    if (keyboard.completed && keyboardTarget_ < 0)
    {
        inputBlocked_ = true;
        keyboard.completed = false;
        return false;
    }
    if (keyboard.active || Transport::wifiDialogActive() || network_.state() == Transport::State::Wifi)
    {
        inputBlocked_ = true;
        heldDirection_ = -1;
        return false;
    }
    if (inputBlocked_)
    {
        inputBlocked_ = menuHeld();
        heldDirection_ = -1;
        return false;
    }
    const int direction = navigation();
    if (mode_ == Mode::List)
    {
        const auto size = servers_.size();
        if (!size)
        {
            actionsFocused_ = true;
            if (action_ == 0 || action_ == 2)
            {
                action_ = 1;
            }
        }
        if (direction == 2 || direction == 3)
        {
            if (!actionsFocused_)
            {
                actionsFocused_ = true;
                action_ = direction == 3 ? 0 : 4;
            }
            else
            {
                do
                {
                    action_ = (action_ + (direction == 3 ? 1 : 4)) % 5;
                } while (!size && (action_ == 0 || action_ == 2));
            }
        }
        else if (direction == 0 && size)
        {
            if (actionsFocused_)
            {
                actionsFocused_ = false;
            }
            else if (selected_)
            {
                --selected_;
            }
            else
            {
                actionsFocused_ = true;
            }
        }
        else if (direction == 1 && size)
        {
            if (actionsFocused_)
            {
                actionsFocused_ = false;
                selected_ = 0;
            }
            else if (selected_ + 1 < size)
            {
                ++selected_;
            }
            else
            {
                actionsFocused_ = true;
            }
        }
        if (IsKeyPressed(KEY_TAB) && size)
        {
            actionsFocused_ = !actionsFocused_;
        }
        const auto first = selected_ < 5 ? 0 : selected_ - 4;
        for (std::size_t row = 0; row < 5 && first + row < size; ++row)
        {
            if (click({10, 50.0f + static_cast<float>(row) * 31, 460, 30}))
            {
                selected_ = first + row;
                actionsFocused_ = false;
            }
        }
        int command = -1;
        if (IsKeyPressed(KEY_ENTER) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
        {
            command = actionsFocused_ ? action_ : 0;
        }
        for (int i = 0; i < 5; ++i)
        {
            if (click({6.0f + i * 94, 218, 88, 26}))
            {
                action_ = i;
                actionsFocused_ = true;
                command = i;
            }
        }
        if (IsKeyPressed(KEY_F5) || pressed(GAMEPAD_BUTTON_MIDDLE_LEFT))
        {
            command = 3;
        }
        else if (IsKeyPressed(KEY_INSERT) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_UP))
        {
            command = 1;
        }
        else if (IsKeyPressed(KEY_F2) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
        {
            command = 2;
        }
        else if (IsKeyPressed(KEY_N) || pressed(GAMEPAD_BUTTON_MIDDLE_RIGHT))
        {
            command = 4;
        }
        if (command >= 0)
        {
            externalNotice.clear();
        }
        if (command == 0 && size)
        {
            if (network_.state() != Transport::State::Open)
            {
                refresh();
            }
            else
            {
                servers_.cancel();
                return true;
            }
        }
        else if (command == 1)
        {
            edit(true);
        }
        else if (command == 2 && size)
        {
            edit(false);
        }
        else if (command == 3)
        {
            refresh();
        }
        else if (command == 4)
        {
            servers_.cancel();
            player_ = servers_.username;
            mode_ = Mode::Player;
            focus_ = 0;
            notice_.clear();
            while (GetCharPressed() > 0)
            {
            }
        }
        else if (pressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) && actionsFocused_ && size)
        {
            actionsFocused_ = false;
        }
        else if (size && (IsKeyPressed(KEY_DELETE) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)))
        {
            externalNotice.clear();
            mode_ = Mode::Remove;
            focus_ = 1;
        }
    }
    else if (mode_ == Mode::Edit || mode_ == Mode::Player)
    {
        const bool player = mode_ == Mode::Player;
        const int fields = player ? 1 : 3;
        if (IsKeyPressed(KEY_TAB) || direction == 1)
        {
            focus_ = (focus_ + 1) % (fields + 2);
        }
        if (direction == 0)
        {
            focus_ = (focus_ + fields + 1) % (fields + 2);
        }
        if (direction == 2 || direction == 3)
        {
            focus_ = focus_ < fields ? fields : fields + (focus_ == fields ? 1 : 0);
        }
        for (int row = 0; row < fields; ++row)
        {
            if (click({124, 67.0f + row * 37, 338, 28}))
            {
                focus_ = row;
            }
        }
        auto &value = player ? player_ : focus_ == 0 ? host_ : focus_ == 1 ? port_ : alias_;
        const int limit = player ? 16 : focus_ == 0 ? 255 : focus_ == 1 ? 5 : 96;
        const bool ascii = player || focus_ != 2;
        const bool digits = !player && focus_ == 1;
        if (focus_ < fields)
        {
            editText(value, limit, ascii, digits);
        }
        const bool cross = pressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
#if defined(__PSP__)
        if (cross && focus_ < fields)
        {
            keyboardTarget_ = player ? 3 : focus_;
            if (!keyboard.open(player        ? "Player name"
                               : focus_ == 0 ? "Server address"
                               : focus_ == 1 ? "Server port"
                                             : "List name (empty = automatic)",
                               value, limit, ascii, digits))
            {
                keyboardTarget_ = -1;
                notice_ = "Cannot open system keyboard";
            }
            return false;
        }
#endif
        if (IsKeyPressed(KEY_ESCAPE) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) || click({248, 205, 214, 28}) ||
            ((cross || IsKeyPressed(KEY_ENTER)) && focus_ == fields + 1))
        {
            mode_ = Mode::List;
            notice_.clear();
        }
        else if (IsKeyPressed(KEY_ENTER) || pressed(GAMEPAD_BUTTON_MIDDLE_RIGHT) || click({18, 205, 214, 28}) ||
                 (cross && focus_ == fields))
        {
            bool saved = false;
            if (player)
            {
                std::array<std::uint8_t, 16> uuid{};
                if (!offlineUuid(player_, uuid))
                {
                    notice_ = "Name: 1-16 letters, digits or underscore";
                }
                else
                {
                    servers_.username = player_;
                    saved = servers_.save();
                    if (!saved)
                    {
                        notice_ = servers_.error();
                    }
                }
            }
            else
            {
                char *end = nullptr;
                const long number = std::strtol(port_.c_str(), &end, 10);
                const bool address =
                    !host_.empty() && host_.size() <= 255 &&
                    host_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") ==
                        std::string::npos;
                if (!address || port_.empty() || !end || *end || number < 1 || number > 65535)
                {
                    notice_ = "Enter a hostname / IPv4 and port 1-65535";
                }
                else
                {
                    ServerEntry entry = editing_ < servers_.size() ? servers_[editing_] : ServerEntry{};
                    if (entry.host != host_ || entry.port != number)
                    {
                        entry.name.clear();
                        entry.description.clear();
                    }
                    entry.host = host_;
                    entry.port = static_cast<std::uint16_t>(number);
                    entry.alias = alias_;
                    saved = servers_.put(editing_, entry);
                    if (saved)
                    {
                        selected_ = editing_;
                    }
                    else
                    {
                        notice_ = servers_.error();
                    }
                }
            }
            if (saved)
            {
                notice_.clear();
                mode_ = Mode::List;
                refreshPending_ = true;
            }
        }
    }
    else if (mode_ == Mode::Remove)
    {
        if (direction >= 0)
        {
            focus_ = 1 - focus_;
        }
        const bool activate = IsKeyPressed(KEY_ENTER) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        if ((activate && focus_ == 0) || IsKeyPressed(KEY_DELETE) || click({18, 180, 214, 28}))
        {
            if (!servers_.erase(selected_))
            {
                notice_ = servers_.error();
            }
            selected_ = servers_.size() ? std::min(selected_, servers_.size() - 1) : 0;
            mode_ = Mode::List;
        }
        else if ((activate && focus_ == 1) || IsKeyPressed(KEY_ESCAPE) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                 click({248, 180, 214, 28}))
        {
            mode_ = Mode::List;
        }
    }
    return false;
}
void ServerBrowser::pump(bool visible)
{
    network_.update();
    if (visible && mode_ == Mode::List && network_.state() == Transport::State::Open)
    {
        if (refreshPending_)
        {
            servers_.refresh();
            refreshPending_ = false;
        }
        servers_.update();
    }
}
void ServerBrowser::draw(Font font, bool connecting, const std::string &notice, bool resources) const
{
    if (Transport::wifiDialogActive() || network_.state() == Transport::State::Wifi)
    {
        DrawRectangle(0, 0, 480, 272, Color{20, 29, 33, 255});
        uiText(font, "Wi-Fi connection", 22, 28, 22, RAYWHITE);
        uiText(font, "Select a saved network using the D-pad.", 22, 76, 16, LIGHTGRAY);
        uiText(font, "Cross: connect   Circle: cancel", 22, 106, 16, LIGHTGRAY);
        uiText(font, "Opening the PSP network dialog...", 22, 170, 14, YELLOW);
        return;
    }
    DrawRectangle(0, 0, 480, 42, {39, 73, 59, 255});
    uiText(font, "CraftNet", 14, 10, 18, RAYWHITE);
    label(font, servers_.username, 300, 13, 166, LIGHTGRAY, 13);
    if (connecting)
    {
        uiText(font, "Connecting to server...", 18, 83, 18, YELLOW);
        uiText(font, "Start / Esc: cancel", 18, 125, 13, LIGHTGRAY);
    }
    else if (mode_ == Mode::List)
    {
        const auto first = selected_ < 5 ? 0 : selected_ - 4;
        for (std::size_t row = 0; row < 5 && first + row < servers_.size(); ++row)
        {
            const auto index = first + row;
            const auto &entry = servers_[index];
            const int y = 50 + static_cast<int>(row) * 31;
            DrawRectangle(10, y, 460, 30, index == selected_ ? Color{51, 75, 63, 255} : Color{28, 40, 41, 255});
            if (index == selected_ && !actionsFocused_)
            {
                DrawRectangleLines(10, y, 460, 30, YELLOW);
            }
            const Color color = entry.status == ServerEntry::Status::Online    ? GREEN
                                : entry.status == ServerEntry::Status::Offline ? ORANGE
                                                                               : LIGHTGRAY;
            label(font, entry.title(), 16, y + 1, 330);
            char population[32];
            std::snprintf(population, sizeof(population), "%d/%d", entry.online, entry.maximum);
            const char *status = entry.status == ServerEntry::Status::Online         ? population
                                 : entry.status == ServerEntry::Status::Incompatible ? "Version"
                                 : entry.status == ServerEntry::Status::Offline      ? "Offline"
                                 : entry.status == ServerEntry::Status::Querying     ? "Fetching"
                                 : entry.status == ServerEntry::Status::Waiting      ? "Waiting"
                                                                                     : "?";
            label(font, status, 363, y + 1, 102, color, 12);
            char address[264];
            const char *description = entry.description.c_str();
            if (entry.description.empty())
            {
                std::snprintf(address, sizeof(address), "%s:%u", entry.host.c_str(), static_cast<unsigned>(entry.port));
                description = address;
            }
            else if (entry.alias.empty())
            {
                const char *newline = std::strchr(description, '\n');
                if (newline && newline[1])
                {
                    description = newline + 1;
                }
            }
            label(font, description, 16, y + 16, 445, LIGHTGRAY, 12);
        }
        if (!servers_.size())
        {
            uiText(font, "No saved servers", 22, 86, 18, RAYWHITE);
            uiText(font, "Triangle / Insert / Add: save a server", 22, 118, 13, LIGHTGRAY);
        }
        button(font, "Join", {6, 218, 88, 26}, servers_.size() && resources, actionsFocused_ && action_ == 0);
        button(font, "Add", {100, 218, 88, 26}, true, actionsFocused_ && action_ == 1);
        button(font, "Edit", {194, 218, 88, 26}, servers_.size() != 0, actionsFocused_ && action_ == 2);
        button(font, "Refresh", {288, 218, 88, 26}, true, actionsFocused_ && action_ == 3);
        button(font, "Player", {382, 218, 88, 26}, true, actionsFocused_ && action_ == 4);
        const char *probeError = servers_.size() && servers_[selected_].status == ServerEntry::Status::Offline
                                     ? servers_[selected_].statusError.c_str()
                                     : nullptr;
        const char *status = !notice.empty()    ? notice.c_str()
                             : !notice_.empty() ? notice_.c_str()
                             : !resources       ? "Missing CCB3 assets; copy the complete build folder"
                             : network_.state() == Transport::State::Wifi   ? "Connect Wi-Fi to refresh saved servers"
                             : network_.state() == Transport::State::Failed ? network_.error().c_str()
                             : !servers_.error().empty()                    ? servers_.error().c_str()
                             : probeError                                   ? probeError
                             : servers_.refreshing() ? "Fetching server names and descriptions..."
                                                     : "D-pad: navigate | Cross: select | Select: refresh";
        label(font, status, 8, 250, 464, LIGHTGRAY, 12);
    }
    else if (mode_ == Mode::Edit || mode_ == Mode::Player)
    {
        const bool player = mode_ == Mode::Player;
        const int fields = player ? 1 : 3;
        field(font, player ? "Player name" : "Address", player ? player_ : host_, 67, focus_ == 0);
        if (!player)
        {
            field(font, "Port", port_, 104, focus_ == 1);
            field(font, "List name", alias_, 141, focus_ == 2);
        }
        uiText(font, player ? "Use letters, digits and underscore" : "Empty list name: use the server name / MOTD", 18, 180,
                 12, LIGHTGRAY);
        button(font, "Start / Enter: Save", {18, 205, 214, 28}, true, focus_ == fields);
        button(font, "Circle / Esc: Cancel", {248, 205, 214, 28}, true, focus_ == fields + 1);
        label(font, notice_.empty() ? "D-pad: field / button | Cross: edit / select" : notice_.c_str(), 18, 246, 444,
              notice_.empty() ? LIGHTGRAY : ORANGE, 12);
    }
    else
    {
        uiText(font, "Remove saved server?", 18, 75, 18, RAYWHITE);
        label(font, servers_[selected_].title(), 18, 108, 444);
        button(font, "Remove", {18, 180, 214, 28}, true, focus_ == 0);
        button(font, "Cancel", {248, 180, 214, 28}, true, focus_ == 1);
        uiText(font, "Left/Right: choose   Cross: select   Circle: back", 18, 237, 12, LIGHTGRAY);
    }
}
} // namespace craft
