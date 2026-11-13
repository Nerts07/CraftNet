#include "session.hpp"
#include "chat.hpp"
#include <raylib.h>
#include <algorithm>
#include <cmath>

namespace craft
{
void Session::clearPresentation()
{
    drops_ = {};
    objectives_.clear();
    scores_.clear();
    scoreSlots_ = {};
    sidebar_ = {};
    sidebarDirty_ = false;
    gameMode_ = 0;
    cameraEntity_ = -1;
    dead_ = respawnRequested_ = immediateRespawn_ = false;
    deathMessage_.clear();
    title_.clear();
    subtitle_.clear();
    actionBar_.clear();
    titleTicks_ = {10, 70, 20};
    titleStart_ = titleEnd_ = actionBarEnd_ = 0;
    handStart_ = -1;
}

void Session::setGameMode(int mode)
{
    gameMode_ = mode;
    if (spectator())
    {
        dead_ = respawnRequested_ = false;
        inventory_.open = false;
        inventory_.container = inventory_.rows = 0;
        inventory_.carried = {};
    }
}

void Session::setDead()
{
    if (spectator())
    {
        return;
    }
    dead_ = true;
    inventory_.open = false;
    inventory_.container = inventory_.rows = 0;
    inventory_.carried = {};
    if (immediateRespawn_)
    {
        requestRespawn();
    }
}

void Session::requestRespawn()
{
    if (state_ == State::Play && dead_ && !spectator() && !respawnRequested_)
    {
        wire::Bytes body;
        wire::var(body, 0); // PERFORM_RESPAWN
        respawnRequested_ = sendPacket(12, body);
    }
}

void Session::swingHand(bool punch)
{
    if (state_ != State::Play || dead_ || spectator())
    {
        return;
    }
    handStart_ = GetTime();
    if (punch)
    {
        sendPacket(46, {});
    }
}

float Session::handSwing() const
{
    return static_cast<float>(std::clamp((GetTime() - handStart_) / 0.30, 0.0, 1.0));
}

float Session::titleAlpha() const
{
    const double now = GetTime();
    if (now >= titleEnd_)
    {
        return 0;
    }
    const double fadeIn = titleTicks_[0] ? (now - titleStart_) * 20 / titleTicks_[0] : 1;
    const double fadeOut = titleTicks_[2] ? (titleEnd_ - now) * 20 / titleTicks_[2] : 1;
    return static_cast<float>(std::clamp(std::min(fadeIn, fadeOut), 0.0, 1.0));
}

bool Session::actionBarVisible() const
{
    return !actionBar_.empty() && GetTime() < actionBarEnd_;
}

const RemotePlayer *Session::cameraTarget() const
{
    if (!spectator() || cameraEntity_ < 0 || cameraEntity_ == entityId_)
    {
        return nullptr;
    }
    for (const auto &player : players_)
    {
        if (player.active && player.id == cameraEntity_)
        {
            return &player;
        }
    }
    return nullptr;
}

bool Session::readEffects(int id, wire::Reader &r)
{
    std::int32_t entity;
    if (id == 25 || id == 34 || id == 43)
    {
        if (id == 34)
        {
            std::uint8_t event;
            if (!r.i32(entity) || !r.byte(event))
            {
                return false;
            }
            if (event != 2)
            {
                return true;
            }
        }
        else if (!r.var(entity))
        {
            return false;
        }
        if (auto *player = findPlayer(entity); player && player->type != remoteType::Item)
        {
            player->hurtUntil = GetTime() + 0.45;
        }
        return true;
    }
    if (id == 123)
    {
        std::int32_t hand, animation, duration;
        if (!r.var(entity) || !r.var(hand) || !r.var(animation) || !r.var(duration))
        {
            return false;
        }
        if (auto *player = findPlayer(entity); player && animation > 0)
        {
            player->offhandSwing = hand == 1;
            player->swingSeconds = std::clamp(static_cast<float>(duration) / 20, 0.05f, 2.0f);
            ++player->swing;
        }
        return true;
    }
    if (id == 127)
    {
        std::int32_t collector, amount;
        if (!r.var(entity) || !r.var(collector) || !r.var(amount) || amount < 0)
        {
            return false;
        }
        if (auto *drop = findPlayer(entity); drop && drop->type == remoteType::Item)
        {
            drop->itemCount = std::max<std::int32_t>(0, drop->itemCount - amount);
            drop->active = drop->itemCount > 0;
        }
        return true;
    }
    if (id == 95)
    {
        if (!r.var(entity))
        {
            return false;
        }
        cameraEntity_ = entity;
        return true;
    }
    if (id == 39)
    {
        std::uint8_t event;
        float value;
        if (!r.byte(event) || !r.f32(value) || !std::isfinite(value))
        {
            return false;
        }
        if (event == 3 && value >= 0 && value <= 3)
        {
            setGameMode(static_cast<int>(value));
        }
        else if (event == 11)
        {
            immediateRespawn_ = value > 0;
            if (immediateRespawn_)
            {
                requestRespawn();
            }
        }
        return true;
    }
    if (id == 69)
    {
        std::string reason;
        if (!r.var(entity) || !readRichChatText(r, reason))
        {
            return false;
        }
        if (entity == entityId_)
        {
            deathMessage_ = std::move(reason);
            setDead();
        }
        return true;
    }
    if (id == 14)
    {
        std::uint8_t reset;
        if (!r.byte(reset))
        {
            return false;
        }
        title_.clear();
        subtitle_.clear();
        titleEnd_ = 0;
        if (reset)
        {
            titleTicks_ = {10, 70, 20};
        }
        return true;
    }
    if (id == 117)
    {
        std::int32_t fadeIn, stay, fadeOut;
        if (!r.i32(fadeIn) || !r.i32(stay) || !r.i32(fadeOut))
        {
            return false;
        }
        const std::int32_t updates[] = {fadeIn, stay, fadeOut};
        for (int i = 0; i < 3; ++i)
        {
            if (updates[i] >= 0)
            {
                titleTicks_[i] = std::min<std::int32_t>(updates[i], 12000);
            }
        }
        if (titleEnd_ > GetTime())
        {
            titleStart_ = GetTime();
            titleEnd_ = titleStart_ + (titleTicks_[0] + titleTicks_[1] + titleTicks_[2]) / 20.0;
        }
        return true;
    }
    std::string text;
    if (!readRichChatText(r, text))
    {
        return false;
    }
    if (id == 116)
    {
        title_ = std::move(text);
        titleStart_ = GetTime();
        titleEnd_ = titleStart_ + (titleTicks_[0] + titleTicks_[1] + titleTicks_[2]) / 20.0;
    }
    else if (id == 114)
    {
        subtitle_ = std::move(text);
    }
    else
    {
        actionBar_ = std::move(text);
        actionBarEnd_ = GetTime() + 2.5;
    }
    return true;
}

bool Session::readEquipment(wire::Reader &r)
{
    std::int32_t entity;
    if (!r.var(entity))
    {
        return false;
    }
    auto *player = findPlayer(entity);
    if (!player || player->type == remoteType::Item)
    {
        return true;
    }
    auto slots = player->equipment;
    auto dyes = player->armorDyes;
    for (int i = 0; i < 16; ++i)
    {
        std::uint8_t encoded;
        ItemStack item;
        if (!r.byte(encoded) || !readItemStack(r, item))
        {
            return false;
        }
        const unsigned slot = encoded & 127;
        if (slot < slots.size())
        {
            slots[slot] = item.id;
            dyes[slot] = item.dye;
        }
        if (!(encoded & 128))
        {
            player->equipment = slots;
            player->armorDyes = dyes;
            return true;
        }
    }
    return false;
}
} // namespace craft
