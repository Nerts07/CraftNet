#pragma once
#include "keyboard.hpp"
#include "servers.hpp"
#include <raylib.h>

namespace craft
{
class ServerBrowser final
{
  public:
    void start();
    // Returns true when Join was activated.
    bool update(Keyboard &keyboard, std::string &externalNotice);
    void pump(bool visible);
    void draw(Font font, bool connecting, const std::string &notice, bool resources) const;
    const ServerEntry &selected() const
    {
        return servers_[selected_];
    }
    const std::string &username() const
    {
        return servers_.username;
    }
    void cancelRefresh()
    {
        servers_.cancel();
    }

  private:
    enum class Mode
    {
        List,
        Edit,
        Player,
        Remove
    };
    void refresh();
    void edit(bool adding);
    int navigation();
    Transport network_;
    ServerList servers_;
    Mode mode_ = Mode::List;
    std::size_t selected_ = 0;
    std::size_t editing_ = 0;
    int focus_ = 0;
    int keyboardTarget_ = -1;
    int action_ = 0;
    int heldDirection_ = -1;
    double repeatAt_ = 0;
    bool actionsFocused_ = false;
    bool inputBlocked_ = false;
    bool refreshPending_ = true;
    std::string host_;
    std::string port_;
    std::string alias_;
    std::string player_;
    std::string notice_;
};
} // namespace craft
