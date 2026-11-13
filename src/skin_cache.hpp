#pragma once
#include "net.hpp"
#include <raylib.h>
#include <array>

namespace craft
{
class SkinCache final
{
  public:
    ~SkinCache();
    void load();
    void beginFrame();
    Texture2D texture(const char *hash, bool slim, bool request);
    void pump();
    void cancel();

  private:
    struct Entry
    {
        std::string hash;
        Texture2D texture{};
        unsigned used = 0;
        double retry = 0;
        bool pending = false;
    };
    bool install(Entry &entry, const unsigned char *data, int size);
    bool finish();
    void stop(bool failed);
    std::array<Entry, 32> entries_{};
    std::array<Texture2D, 2> defaults_{};
    Transport transport_;
    std::vector<std::uint8_t> received_;
    unsigned frame_ = 0;
    int current_ = -1;
    bool requested_ = false;
    double deadline_ = 0;
};
} // namespace craft
