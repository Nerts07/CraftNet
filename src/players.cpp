#include "session.hpp"
#include "chat.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace craft
{
namespace
{
bool uuid(wire::Reader &r, std::array<std::uint8_t, 16> &value)
{
    if (r.remaining() < value.size())
    {
        return false;
    }
    std::copy_n(r.data(), value.size(), value.begin());
    return r.skip(value.size());
}
bool bytes(wire::Reader &r, int maximum)
{
    std::int32_t size;
    return r.var(size) && size >= 0 && size <= maximum && r.skip(static_cast<std::size_t>(size));
}
std::string skinHash(const std::string &encoded, bool &slim)
{
    if (encoded.size() > 16384)
    {
        return {};
    }
    std::string json;
    json.reserve(encoded.size() * 3 / 4);
    unsigned bits = 0, accumulator = 0;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (char c : encoded)
    {
        if (c == '=')
        {
            break;
        }
        const char *digit = c ? std::strchr(alphabet, c) : nullptr;
        if (!digit)
        {
            return {};
        }
        accumulator = (accumulator << 6) | static_cast<unsigned>(digit - alphabet);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            json.push_back(static_cast<char>((accumulator >> bits) & 255));
        }
    }
    auto at = json.find("\"SKIN\"");
    if (at == std::string::npos)
    {
        return {};
    }
    const auto end = json.find("\"CAPE\"", at);
    auto url = json.find("textures.minecraft.net/texture/", at);
    if (url == std::string::npos || (end != std::string::npos && url > end))
    {
        return {};
    }
    // Accept only the exact official host, never a server-selected arbitrary URL.
    if (url < 7 || (json.compare(url - 7, 7, "http://") && (url < 8 || json.compare(url - 8, 8, "https://"))))
    {
        return {};
    }
    url += std::strlen("textures.minecraft.net/texture/");
    const auto last = json.find('"', url);
    if (last == std::string::npos || last - url > 64 || last - url < 32)
    {
        return {};
    }
    auto hash = json.substr(url, last - url);
    if (hash.find_first_not_of("0123456789abcdef") != std::string::npos)
    {
        return {};
    }
    const auto model = json.find("\"slim\"", last);
    slim = model != std::string::npos && (end == std::string::npos || model < end);
    return hash;
}

bool readSkinProperties(wire::Reader &r, std::string &skin, bool &slim)
{
    std::int32_t count;
    if (!r.var(count) || count < 0 || count > 64)
    {
        return false;
    }
    for (int i = 0; i < count; ++i)
    {
        std::string name, value, signature;
        std::uint8_t signedValue;
        if (!r.str(name, 256) || !r.str(value, 16384) || !r.byte(signedValue) || signedValue > 1 ||
            (signedValue && !r.str(signature, 8192)))
        {
            return false;
        }
        if (name == "textures")
        {
            skin = skinHash(value, slim);
        }
    }
    return true;
}

bool readMannequinProfile(wire::Reader &r, std::string &name, std::string &skin, bool &slim)
{
    std::uint8_t full, present;
    if (!r.byte(full))
    {
        return false;
    }
    if (full)
    {
        if (!r.skip(16) || !r.str(name, 64))
        {
            return false;
        }
    }
    else if (!r.byte(present) || (present && !r.str(name, 64)) || !r.byte(present) || (present && !r.skip(16)))
    {
        return false;
    }
    if (!readSkinProperties(r, skin, slim))
    {
        return false;
    }
    // Skin patches can reference resource-pack textures. Consume the IDs and
    // keep the public profile texture, if supplied.
    for (int i = 0; i < 3; ++i)
    {
        std::string resource;
        if (!r.byte(present) || (present && !r.str(resource, 256)))
        {
            return false;
        }
    }
    std::uint8_t model;
    if (!r.byte(present) || (present && !r.byte(model)))
    {
        return false;
    }
    if (present)
    {
        slim = model != 0;
    }
    return true;
}
template <std::size_t N> void copy(std::array<char, N> &out, const std::string &in)
{
    out.fill(0);
    auto length = std::min(in.size(), N - 1);
    while (length && length < in.size() && (static_cast<unsigned char>(in[length]) & 0xc0) == 0x80)
    {
        --length;
    }
    std::copy_n(in.data(), length, out.data());
}
} // namespace

void Session::resetPlayerInfo()
{
    profiles_ = {};
    ++rosterRevision_;
    teams_ = {};
    members_ = {};
    profileAge_ = 0;
    labelsDirty_ = false;
}
void Session::syncPlayer(RemotePlayer &player)
{
    if (player.type == remoteType::TextDisplay)
    {
        return;
    }
    const Profile *profile = nullptr;
    if (player.type == remoteType::Player)
    {
        for (const auto &p : profiles_)
        {
            if (p.used && p.uuid == player.uuid)
            {
                profile = &p;
                break;
            }
        }
    }
    if (profile)
    {
        copy(player.account, profile->account);
        copy(player.skin, profile->skin);
        player.slim = profile->slim;
    }
    int team = -1, own = -1;
    for (const auto &member : members_)
    {
        if (member.account == player.account.data() && !member.account.empty())
        {
            team = member.team;
        }
        if (member.account == username_)
        {
            own = member.team;
        }
    }
    std::string label = player.name[0] ? player.name.data() : player.account.data();
    player.spectator = profile && profile->spectator;
    player.hideLabel = player.spectator;
    if (team >= 0 && team < static_cast<int>(teams_.size()) && teams_[team].used)
    {
        const auto &t = teams_[team];
        const auto color = std::string("\xc2\xa7") + "0123456789abcdef"[std::clamp(t.color, 0, 15)];
        label = color + t.prefix + color + label + color + t.suffix;
        player.hideLabel |=
            t.visibility == 1 || (t.visibility == 2 && team != own) || (t.visibility == 3 && team == own);
    }
    else if (!player.name[0] && profile && !profile->display.empty())
    {
        label = profile->display;
    }
    if (player.description[0])
    {
        if (!label.empty())
        {
            label += '\n';
        }
        label += player.description.data();
    }
    copy(player.label, label);
}
void Session::refreshLabels()
{
    ++rosterRevision_;
    for (auto &player : players_)
    {
        if (player.active)
        {
            syncPlayer(player);
        }
    }
}

std::vector<std::string> Session::tabPlayers() const
{
    struct Entry
    {
        const Profile *profile;
        const Team *team;
    };
    std::vector<Entry> entries;
    for (const auto &profile : profiles_)
    {
        if (!profile.used || !profile.listed || profile.account.empty())
        {
            continue;
        }
        const Team *team = nullptr;
        for (const auto &member : members_)
        {
            if (member.team >= 0 && member.account == profile.account && teams_[member.team].used)
            {
                team = &teams_[member.team];
                break;
            }
        }
        entries.push_back({&profile, team});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b)
    {
        if (a.profile->listOrder != b.profile->listOrder)
        {
            return a.profile->listOrder > b.profile->listOrder;
        }
        if (a.profile->spectator != b.profile->spectator)
        {
            return !a.profile->spectator;
        }
        const std::string_view teamA = a.team ? std::string_view(a.team->id) : std::string_view{};
        const std::string_view teamB = b.team ? std::string_view(b.team->id) : std::string_view{};
        if (teamA != teamB)
        {
            return teamA < teamB;
        }
        const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        return std::lexicographical_compare(a.profile->account.begin(), a.profile->account.end(),
                                            b.profile->account.begin(), b.profile->account.end(),
                                            [&](unsigned char x, unsigned char y) { return fold(x) < fold(y); });
    });
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto &entry : entries)
    {
        const auto &profile = *entry.profile;
        if (profile.hasDisplay)
        {
            names.push_back(profile.display);
        }
        else if (entry.team)
        {
            const auto &team = *entry.team;
            const auto color = std::string("\xc2\xa7") + "0123456789abcdef"[std::clamp(team.color, 0, 15)];
            names.push_back(color + team.prefix + color + profile.account + color + team.suffix);
        }
        else
        {
            names.push_back(profile.account);
        }
    }
    return names;
}

bool Session::readEntityMetadata(wire::Reader &r)
{
    std::int32_t entity;
    if (!r.var(entity))
    {
        return false;
    }
    auto *p = findPlayer(entity);
    if (!p)
    {
        return true;
    }
    for (int i = 0; i < 64; ++i)
    {
        std::uint8_t index, byte;
        std::int32_t type, value;
        std::uint64_t number;
        if (!r.byte(index))
        {
            return false;
        }
        if (index == 255)
        {
            return true;
        }
        if (!r.var(type))
        {
            return false;
        }
        switch (type)
        {
        case 7:
        {
            ItemStack item;
            if (!readItemStack(r, item))
            {
                return false;
            }
            if (p->type == remoteType::Item && index == 8)
            {
                p->equipment[0] = item.id;
                p->itemCount = item.count;
            }
            break;
        }
        case 0:
        case 8:
            if (!r.byte(byte))
            {
                return false;
            }
            if (type == 0 && index == 0)
            {
                p->invisible = (byte & 32) != 0;
                p->sneaking = (byte & 2) != 0;
            }
            if (type == 8 && index == 3)
            {
                p->customNameVisible = byte != 0;
            }
            if (type == 0 && (p->type == remoteType::Player || p->type == remoteType::Mannequin) && index == 16)
            {
                p->skinParts = byte;
            }
            if (type == 0 && p->type == remoteType::ArmorStand && index == 15)
            {
                p->small = (byte & 1) != 0;
                p->marker = (byte & 16) != 0;
            }
            break;
        case 1:
        case 12:
        case 14:
        case 15:
        case 19:
        case 20:
        case 42:
            if (!r.var(value))
            {
                return false;
            }
            if (type == 20 && index == 6)
            {
                p->sneaking = value == 5;
            }
            break;
        case 2:
            if (!r.varlong(number))
            {
                return false;
            }
            break;
        case 3:
        {
            float length;
            if (!r.f32(length))
            {
                return false;
            }
            if (p->type == remoteType::Interaction && std::isfinite(length))
            {
                if (index == 8)
                {
                    p->width = std::clamp(length, 0.0f, 32.0f);
                }
                if (index == 9)
                {
                    p->height = std::clamp(length, 0.0f, 32.0f);
                }
            }
            break;
        }
        case 5:
        case 6:
        {
            byte = 1;
            std::string text;
            if ((type == 6 && !r.byte(byte)) || (byte && !readRichChatText(r, text)))
            {
                return false;
            }
            if (p->type == remoteType::TextDisplay && index == 23)
            {
                copy(p->label, text);
            }
            else if (index == 2)
            {
                copy(p->name, text);
                syncPlayer(*p);
            }
            else if (p->type == remoteType::Mannequin && index == 19)
            {
                copy(p->description, text);
                syncPlayer(*p);
            }
            break;
        }
        case 9:
        case 39:
        {
            std::array<float, 3> vector;
            if (!r.f32(vector[0]) || !r.f32(vector[1]) || !r.f32(vector[2]))
            {
                return false;
            }
            if (p->type == remoteType::TextDisplay && type == 39 &&
                std::all_of(vector.begin(), vector.end(), [](float v) { return std::isfinite(v); }))
            {
                if (index == 11)
                {
                    p->labelOffset = vector;
                }
                if (index == 12)
                {
                    p->labelScale = std::clamp(std::abs(vector[1]), 0.05f, 8.0f);
                }
            }
            break;
        }
        case 10:
        case 11:
            byte = 1;
            if ((type == 11 && !r.byte(byte)) || (byte && !r.skip(8)))
            {
                return false;
            }
            break;
        case 17:
            // A zero-length particle list is common in full NPC metadata.
            if (!r.var(value) || value != 0)
            {
                return false;
            }
            break;
        case 40:
            if (!r.skip(16))
            {
                return false;
            }
            break;
        case 41:
        {
            std::string name, skin;
            bool slim = false;
            if (!readMannequinProfile(r, name, skin, slim))
            {
                return false;
            }
            if (p->type == remoteType::Mannequin && index == 17)
            {
                copy(p->account, name);
                copy(p->skin, skin);
                p->slim = slim;
                syncPlayer(*p);
            }
            break;
        }
        default:
            // The transport already consumed this frame. Unknown serializers
            // have no common length, so the rest cannot be read safely.
            return false;
        }
    }
    return false;
}

bool Session::handlePlayers(int id, wire::Reader &r)
{
    if (id == 2 || id == 85)
    {
        std::int32_t entity;
        std::uint8_t action;
        if (!r.var(entity) || !r.byte(action))
        {
            return false;
        }
        if (auto *player = findPlayer(entity))
        {
            if (id == 85)
            {
                player->headYaw = action * (360.0f / 256);
            }
            else if (action == 0 || action == 3)
            {
                ++player->swing;
                player->swingSeconds = 0.3f;
                player->offhandSwing = action == 3;
            }
        }
        return true;
    }
    if (id == 70)
    {
        std::int32_t count;
        if (!r.var(count) || count < 0 || count > 4096)
        {
            return false;
        }
        for (int i = 0; i < count; ++i)
        {
            std::array<std::uint8_t, 16> key;
            if (!uuid(r, key))
            {
                return false;
            }
            // Keep skin/account while its entity remains alive (common for NPCs
            // removed from the tab list immediately after spawning).
            bool alive = false;
            for (const auto &player : players_)
            {
                alive |= player.active && player.uuid == key;
            }
            for (auto &p : profiles_)
            {
                if (p.used && p.uuid == key)
                {
                    p.listed = false;
                    if (!alive)
                    {
                        p = {};
                    }
                    labelsDirty_ = true;
                    break;
                }
            }
        }
        return true;
    }
    if (id == 71)
    {
        std::uint8_t actions;
        std::int32_t count;
        if (!r.byte(actions))
        {
            return false;
        }
        if (!(actions & (1 | 4 | 8 | 32 | 64)))
        {
            return true;
        }
        if (!r.var(count) || count < 0 || count > 4096)
        {
            return false;
        }
        for (int i = 0; i < count; ++i)
        {
            std::array<std::uint8_t, 16> key;
            if (!uuid(r, key))
            {
                return false;
            }
            Profile update;
            if (actions & 1)
            {
                if (!r.str(update.account, 64) || !readSkinProperties(r, update.skin, update.slim))
                {
                    return false;
                }
            }
            if (actions & 2)
            {
                std::uint8_t present;
                if (!r.byte(present) || present > 1 || (present && (!r.skip(24) || !bytes(r, 8192) || !bytes(r, 4096))))
                {
                    return false;
                }
            }
            std::int32_t value;
            std::uint8_t flag;
            if (actions & 4)
            {
                if (!r.var(value))
                {
                    return false;
                }
                update.spectator = value == 3;
            }
            if ((actions & 8) && (!r.byte(flag) || flag > 1))
            {
                return false;
            }
            if (actions & 8)
            {
                update.listed = flag != 0;
            }
            if ((actions & 16) && !r.var(value))
            {
                return false;
            }
            if (actions & 32)
            {
                if (!r.byte(flag) || flag > 1)
                {
                    return false;
                }
                if (flag && !readRichChatText(r, update.display))
                {
                    return false;
                }
                update.hasDisplay = flag != 0;
            }
            if ((actions & 64) && !r.var(value))
            {
                return false;
            }
            if (actions & 64)
            {
                update.listOrder = value;
            }
            if ((actions & 128) && (!r.byte(flag) || flag > 1))
            {
                return false;
            }
            auto p = std::find_if(profiles_.begin(), profiles_.end(), [&](const Profile &candidate)
                                  { return candidate.used && candidate.uuid == key; });
            if (p == profiles_.end())
            {
                if (!(actions & 1))
                {
                    continue;
                }
                p = std::min_element(profiles_.begin(), profiles_.end(),
                                     [](const Profile &a, const Profile &b) { return a.age < b.age; });
                if (p == profiles_.end() || (p->used && profiles_.size() < 2048))
                {
                    profiles_.emplace_back();
                    p = profiles_.end() - 1;
                }
                *p = {};
                p->uuid = key;
                p->used = true;
            }
            p->age = ++profileAge_;
            if (actions & 1)
            {
                p->account = std::move(update.account);
                p->skin = std::move(update.skin);
                p->slim = update.slim;
            }
            if (actions & 4)
            {
                p->spectator = update.spectator;
            }
            if (actions & 32)
            {
                p->display = std::move(update.display);
                p->hasDisplay = update.hasDisplay;
            }
            if (actions & 8)
            {
                p->listed = update.listed;
            }
            if (actions & 64)
            {
                p->listOrder = update.listOrder;
            }
            labelsDirty_ = true;
        }
        return r.remaining() == 0;
    }
    std::string name;
    std::uint8_t method;
    if (!r.str(name, 64) || !r.byte(method) || method > 4)
    {
        return false;
    }
    int index = -1;
    for (std::size_t i = 0; i < teams_.size(); ++i)
    {
        if (teams_[i].used && teams_[i].id == name)
        {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0 && method == 0)
    {
        for (std::size_t i = 0; i < teams_.size(); ++i)
        {
            if (!teams_[i].used)
            {
                index = static_cast<int>(i);
                teams_[i] = {};
                teams_[i].used = true;
                teams_[i].id = name;
                break;
            }
        }
    }
    if (method == 1)
    {
        if (index >= 0)
        {
            teams_[index] = {};
            for (auto &member : members_)
            {
                if (member.team == index)
                {
                    member = {};
                }
            }
        }
    }
    if (method == 0 || method == 2)
    {
        std::string display, prefix, suffix;
        std::int32_t visibility, collision, color = 15;
        std::uint8_t present, options;
        if (!readRichChatText(r, display) || !readRichChatText(r, prefix) || !readRichChatText(r, suffix) ||
            !r.var(visibility) || !r.var(collision) || !r.byte(present) || present > 1 || (present && !r.var(color)) ||
            !r.byte(options))
        {
            return false;
        }
        if (index >= 0)
        {
            teams_[index].prefix = prefix.substr(0, 128);
            teams_[index].suffix = suffix.substr(0, 128);
            teams_[index].visibility = visibility;
            teams_[index].color = color;
        }
    }
    if (method == 0 || method == 3 || method == 4)
    {
        std::int32_t count;
        if (!r.var(count) || count < 0 || count > 4096)
        {
            return false;
        }
        for (int i = 0; i < count; ++i)
        {
            std::string account;
            if (!r.str(account, 64))
            {
                return false;
            }
            Membership *member = nullptr;
            for (auto &candidate : members_)
            {
                if (candidate.account == account)
                {
                    member = &candidate;
                    break;
                }
                if (!member && candidate.account.empty())
                {
                    member = &candidate;
                }
            }
            if (member && index >= 0)
            {
                if (method == 4)
                {
                    if (member->account == account && member->team == index)
                    {
                        *member = {};
                    }
                }
                else
                {
                    member->account = account;
                    member->team = index;
                }
            }
        }
    }
    labelsDirty_ = true;
    return r.remaining() == 0;
}
} // namespace craft
