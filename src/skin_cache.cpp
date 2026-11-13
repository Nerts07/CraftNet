#include "skin_cache.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace craft
{
namespace
{
constexpr std::size_t MaxResponse = 72 * 1024;
Texture2D upload(Image image)
{
    if (!image.data)
    {
        return {};
    }
#if defined(__PSP__)
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R4G4B4A4);
#endif
    auto texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (texture.id)
    {
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    }
    return texture;
}
void path(char *out, int slot, const char *extension)
{
    std::snprintf(out, 96, "skin-cache/%d.%s", slot, extension);
}
} // namespace
SkinCache::~SkinCache()
{
    cancel();
    for (auto &entry : entries_)
    {
        if (entry.texture.id)
        {
            UnloadTexture(entry.texture);
        }
    }
    for (auto texture : defaults_)
    {
        if (texture.id)
        {
            UnloadTexture(texture);
        }
    }
}
void SkinCache::load()
{
    defaults_[0] = upload(LoadImage("assets/imported/compiled/steve.png"));
    defaults_[1] = upload(LoadImage("assets/imported/compiled/alex.png"));
    MakeDirectory("skin-cache");
    for (std::size_t i = 0; i < entries_.size(); ++i)
    {
        char file[96], hash[66]{};
        path(file, static_cast<int>(i), "hash");
        if (FILE *input = std::fopen(file, "rb"))
        {
            const auto size = std::fread(hash, 1, 65, input);
            std::fclose(input);
            const std::string key(hash, size);
            if (size >= 32 && size <= 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos)
            {
                entries_[i].hash = key;
            }
        }
    }
}
void SkinCache::beginFrame()
{
    ++frame_;
}
bool SkinCache::install(Entry &entry, const unsigned char *data, int size)
{
    if (size < 33 || size > 65536 || std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) || std::memcmp(data + 12, "IHDR", 4))
    {
        return false;
    }
    const auto read = [](const unsigned char *p)
    {
        return (unsigned(p[0]) << 24) | (unsigned(p[1]) << 16) | (unsigned(p[2]) << 8) | p[3];
    };
    const auto height = read(data + 20);
    if (read(data + 16) != 64 || (height != 32 && height != 64))
    {
        return false;
    }
    Image image = LoadImageFromMemory(".png", data, size);
    if (!image.data || image.width != 64 || image.height != static_cast<int>(height))
    {
        UnloadImage(image);
        return false;
    }
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (height == 32)
    {
        Image modern = GenImageColor(64, 64, BLANK);
        ImageDraw(&modern, image, {0, 0, 64, 32}, {0, 0, 64, 32}, WHITE);
        const auto *source = static_cast<const Color *>(image.data);
        auto *target = static_cast<Color *>(modern.data);
        bool opaqueHat = true;
        for (int y = 0; y < 16; ++y)
        {
            for (int x = 32; x < 64; ++x)
            {
                opaqueHat &= source[y * 64 + x].a >= 128;
            }
        }
        if (opaqueHat)
        {
            for (int y = 0; y < 16; ++y)
            {
                for (int x = 32; x < 64; ++x)
                {
                    target[y * 64 + x].a = 0;
                }
            }
        }
        // Mirror each legacy limb's box net, including swapping its sides.
        const int faces[6][4] = {{0, 4, 4, 12},  {4, 4, 4, 12}, {8, 4, 4, 12},
                                 {12, 4, 4, 12}, {4, 0, 4, 4},  {8, 0, 4, 4}};
        for (int limb = 0; limb < 2; ++limb)
        {
            const int oldU = limb ? 40 : 0;
            const int newU = limb ? 32 : 16;
            for (int face = 0; face < 6; ++face)
            {
                const int from = face == 0 ? 2 : face == 2 ? 0 : face;
                for (int y = 0; y < faces[face][3]; ++y)
                {
                    for (int x = 0; x < faces[face][2]; ++x)
                    {
                        target[(48 + faces[face][1] + y) * 64 + newU + faces[face][0] + x] =
                            source[(16 + faces[from][1] + y) * 64 + oldU + faces[from][0] + 3 - x];
                    }
                }
            }
        }
        UnloadImage(image);
        image = modern;
    }
    // Minecraft's base skin layer is opaque; transparency belongs to overlays.
    auto *pixels = static_cast<Color *>(image.data);
    const int opaque[6][4] = {{0, 0, 32, 16},   {0, 16, 16, 16},  {16, 16, 24, 16},
                              {40, 16, 16, 16}, {16, 48, 16, 16}, {32, 48, 16, 16}};
    for (const auto &rect : opaque)
    {
        for (int y = rect[1]; y < rect[1] + rect[3]; ++y)
        {
            for (int x = rect[0]; x < rect[0] + rect[2]; ++x)
            {
                pixels[y * 64 + x].a = 255;
            }
        }
    }
    entry.texture = upload(image);
    return entry.texture.id != 0;
}
Texture2D SkinCache::texture(const char *hash, bool slim, bool request)
{
    auto fallback = defaults_[slim ? 1 : 0];
    if (!hash || !*hash)
    {
        return fallback;
    }
    Entry *entry = nullptr;
    for (auto &candidate : entries_)
    {
        if (candidate.hash == hash)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry && request)
    {
        for (auto &candidate : entries_)
        {
            if (!candidate.pending && candidate.used + 2 < frame_ && (!entry || candidate.used < entry->used))
            {
                entry = &candidate;
            }
        }
        if (entry)
        {
            if (entry->texture.id)
            {
                UnloadTexture(entry->texture);
            }
            *entry = {};
            entry->hash = hash;
            entry->pending = true;
        }
    }
    if (!entry)
    {
        return fallback;
    }
    entry->used = frame_;
    if (!entry->texture.id && !entry->pending && request && GetTime() >= entry->retry)
    {
        entry->pending = true;
    }
    return entry->texture.id ? entry->texture : fallback;
}
void SkinCache::stop(bool failed)
{
    if (current_ >= 0)
    {
        entries_[current_].pending = false;
        entries_[current_].retry = GetTime() + (failed ? 60 : 0);
    }
    transport_.close();
    current_ = -1;
    requested_ = false;
    received_.clear();
}
void SkinCache::cancel()
{
    stop(false);
    for (auto &entry : entries_)
    {
        entry.pending = false;
    }
}
bool SkinCache::finish()
{
    const std::string response(received_.begin(), received_.end());
    const auto header = response.find("\r\n\r\n");
    if (header == std::string::npos || header > 4096 ||
        (response.compare(0, 12, "HTTP/1.1 200") && response.compare(0, 12, "HTTP/1.0 200")))
    {
        return false;
    }
    const auto offset = header + 4;
    if (!install(entries_[current_], received_.data() + offset, static_cast<int>(received_.size() - offset)))
    {
        return false;
    }
    char file[96];
    path(file, current_, "png");
    if (SaveFileData(file, received_.data() + offset, static_cast<int>(received_.size() - offset)))
    {
        path(file, current_, "hash");
        if (FILE *output = std::fopen(file, "wb"))
        {
            const auto &hash = entries_[current_].hash;
            std::fwrite(hash.data(), 1, hash.size(), output);
            std::fclose(output);
        }
    }
    return true;
}
void SkinCache::pump()
{
    if (Transport::wifiDialogActive())
    {
        return;
    }
    if (current_ < 0)
    {
        for (std::size_t i = 0; i < entries_.size(); ++i)
        {
            auto &entry = entries_[i];
            if (!entry.pending)
            {
                continue;
            }
            current_ = static_cast<int>(i);
            char file[96];
            path(file, current_, "hash");
            char hash[66]{};
            bool matches = false;
            if (FILE *input = std::fopen(file, "rb"))
            {
                const auto count = std::fread(hash, 1, 65, input);
                std::fclose(input);
                matches = entry.hash == std::string(hash, count);
            }
            path(file, current_, "png");
            if (matches && GetFileLength(file) > 0 && GetFileLength(file) <= 65536)
            {
                int size = 0;
                auto *data = LoadFileData(file, &size);
                const bool loaded = data && install(entry, data, size);
                UnloadFileData(data);
                if (loaded)
                {
                    stop(false);
                    return;
                }
            }
            received_.clear();
            received_.reserve(MaxResponse);
            deadline_ = GetTime() + 15;
            if (!transport_.begin("textures.minecraft.net", 80))
            {
                stop(true);
            }
            return;
        }
        return;
    }
    transport_.update();
    if (GetTime() > deadline_ || transport_.state() == Transport::State::Failed)
    {
        stop(true);
        return;
    }
    if (transport_.state() != Transport::State::Open)
    {
        return;
    }
    if (!requested_)
    {
        const auto request =
            "GET /texture/" + entries_[current_].hash +
            " HTTP/1.0\r\nHost: textures.minecraft.net\r\nConnection: close\r\nAccept: image/png\r\n\r\n";
        if (!transport_.queue(reinterpret_cast<const std::uint8_t *>(request.data()), request.size()))
        {
            stop(true);
            return;
        }
        requested_ = true;
    }
    std::uint8_t data[4096];
    for (int i = 0; i < 2; ++i)
    {
        const int count = transport_.receive(data, sizeof(data));
        if (count < 0)
        {
            const bool loaded = finish();
            stop(!loaded);
            return;
        }
        if (!count)
        {
            break;
        }
        if (received_.size() + static_cast<unsigned>(count) > MaxResponse)
        {
            stop(true);
            return;
        }
        received_.insert(received_.end(), data, data + count);
    }
}
} // namespace craft
