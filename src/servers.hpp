#pragma once
#include "net.hpp"
#include <array>
#include <string>
#include <vector>

namespace craft
{
struct ServerEntry
{
    enum class Status
    {
        Unknown,
        Waiting,
        Querying,
        Online,
        Offline,
        Incompatible
    };
    std::string host;
    std::string alias;
    std::string name;
    std::string description;
    std::string statusError;
    std::uint16_t port = 25565;
    int online = 0;
    int maximum = 0;
    int protocol = 0;
    Status status = Status::Unknown;
    const std::string &title() const
    {
        return !alias.empty() ? alias : !name.empty() ? name : host;
    }
};

class ServerList final
{
  public:
    static constexpr std::size_t Capacity = 24;
    void load();
    bool save();
    bool put(std::size_t index, const ServerEntry &entry);
    bool erase(std::size_t index);
    void refresh();
    void cancel();
    void update();
    bool refreshing() const
    {
        return refreshing_;
    }
    std::size_t size() const
    {
        return count_;
    }
    const ServerEntry &operator[](std::size_t i) const
    {
        return entries_[i];
    }
    std::string username = "PSPPlayer";
    const std::string &error() const
    {
        return error_;
    }

  private:
    void finish(const char *failure = nullptr);
    std::array<ServerEntry, Capacity> entries_{};
    std::size_t count_ = 0;
    std::size_t current_ = 0;
    Transport query_;
    std::vector<std::uint8_t> response_;
    double deadline_ = 0;
    bool refreshing_ = false;
    bool requested_ = false;
    bool dirty_ = false;
    std::string error_;
};
} // namespace craft
