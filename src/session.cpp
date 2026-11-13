#include "session.hpp"
#include "offline_uuid.hpp"
#include "chat.hpp"
#include "text.hpp"
#include "dimension_nbt.hpp"
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <zlib.h>

namespace craft
{
namespace
{
// 26.3 IDs correspond to Mojang's generated protocol report.
constexpr int cbKeep = 45, cbChunk = 46, cbLogin = 50, cbPos = 73;
#if defined(__PSP__)
constexpr double NetworkFrameBudget = 0.002;
#else
constexpr double NetworkFrameBudget = 0.004;
#endif
// Returns whether the reader has consumed all bytes.
bool empty(const wire::Reader &r)
{
    return r.remaining() == 0;
}
// Sign-extends the low bits of a packed value.
std::int32_t signedBits(std::uint64_t v, unsigned bits)
{
    const auto sign = std::uint64_t(1) << (bits - 1);
    return static_cast<std::int32_t>((v ^ sign) - sign);
}
} // namespace

// Closes the connection and resets session state.
void Session::disconnect()
{
    resetPlayerInfo();
    inventory_ = {};
    clearPresentation();
    dimensions_ = {};
    dimensionKey_.clear();
    cookies_ = {};
    nextCookie_ = 0;
    transferring_ = false;
    sentSprint_ = false;
    sentInput_ = 0xff;
    transport_.close();
    decoder_.reset();
    world_.clear();
    players_ = {};
    chat_ = {};
    player_ = {};
    state_ = State::Idle;
    compression_ = -1;
    hasPosition_ = loaded_ = false;
    sequence_ = 0;
    chatOffset_ = 0;
    chatSeen_ = 0;
}

// Records a fatal session error.
void Session::fail(const char *message)
{
    error_ = message;
    if (FILE *file = std::fopen("last_error.txt", "wb"))
    {
        std::fwrite(error_.data(), 1, error_.size(), file);
        std::fclose(file);
    }
    transport_.close();
    decoder_.reset();
    world_.clear();
    players_ = {};
    state_ = State::Failed;
}

// Starts a connection and offline-mode login.
bool Session::connect(const std::string &host, std::uint16_t port, const std::string &username)
{
    disconnect();
    error_.clear();
    std::array<std::uint8_t, 16> uuid{};
    if (!offlineUuid(username, uuid))
    {
        fail("Name must contain 1-16 letters, digits or underscore");
        return false;
    }
    if (!transport_.begin(host, port))
    {
        error_ = transport_.error();
        state_ = State::Failed;
        return false;
    }
    host_ = host;
    port_ = port;
    username_ = username;
    state_ = State::Connecting;
    deadline_ = GetTime() + 60.0;
    return true;
}

// Frames and sends one protocol packet.
bool Session::sendPacket(int id, const wire::Bytes &body)
{
    wire::Bytes raw;
    raw.reserve(body.size() + 5);
    wire::var(raw, id);
    wire::bytes(raw, body.data(), body.size());
    if (raw.size() > protocol::MaxPacketBytes)
    {
        fail("Packet too large");
        return false;
    }
    wire::Bytes payload;
    if (compression_ >= 0)
    {
        if (raw.size() >= static_cast<std::size_t>(compression_))
        {
            wire::var(payload, static_cast<std::int32_t>(raw.size()));
            uLongf n = compressBound(static_cast<uLong>(raw.size()));
            auto offset = payload.size();
            payload.resize(offset + n);
            if (compress2(payload.data() + offset, &n, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) !=
                Z_OK)
            {
                fail("Compression failed");
                return false;
            }
            payload.resize(offset + n);
        }
        else
        {
            wire::var(payload, 0);
            wire::bytes(payload, raw.data(), raw.size());
        }
    }
    else
    {
        payload = std::move(raw);
    }
    wire::Bytes frame;
    wire::var(frame, static_cast<std::int32_t>(payload.size()));
    wire::bytes(frame, payload.data(), payload.size());
    if (!transport_.queue(frame.data(), frame.size()))
    {
        fail("Send queue failed");
        return false;
    }
    return true;
}

// Processes network input and server packets.
void Session::update()
{
    if (state_ == State::Idle || state_ == State::Failed)
    {
        return;
    }
    // Wi-Fi utility rendering belongs to flush(), inside the drawing frame.
    if (transport_.state() != Transport::State::Wifi)
    {
        transport_.update();
    }
    if (transport_.state() == Transport::State::Failed)
    {
        auto message = transport_.error();
        fail(message.c_str());
        return;
    }
    if (state_ == State::Connecting && transport_.state() == Transport::State::Open)
    {
        wire::Bytes handshake;
        std::array<std::uint8_t, 16> uuid{};
        if (!protocol::handshake(host_, port_, transferring_ ? 3 : 2, handshake) ||
            !transport_.queue(handshake.data(), handshake.size()) || !offlineUuid(username_, uuid))
        {
            fail("Cannot start login");
            return;
        }
        wire::Bytes body;
        wire::string(body, username_);
        wire::bytes(body, uuid.data(), uuid.size());
        if (!sendPacket(0, body))
        {
            return;
        }
        state_ = State::Login;
        deadline_ = GetTime() + 30.0;
    }
    if (state_ != State::Play && GetTime() > deadline_)
    {
        fail("Server login timed out");
        return;
    }
    if (transport_.state() != Transport::State::Open)
    {
        return;
    }
    // Drain buffered frames before receiving more; all borrowed payloads are
    // consumed synchronously before another append/next call can invalidate them.
    const double networkDeadline = GetTime() + NetworkFrameBudget;
    for (int iteration = 0; iteration < 256 && (iteration == 0 || GetTime() < networkDeadline); ++iteration)
    {
        std::int32_t id = 0;
        const std::uint8_t *data = nullptr;
        std::size_t size = 0;
        auto result = decoder_.next(id, data, size);
        if (result == protocol::DecodeResult::Packet)
        {
            if (!handle(id, data, size))
            {
                if (state_ != State::Failed)
                {
                    char message[128];
                    std::snprintf(message, sizeof(message), "Invalid packet: state=%d id=%d bytes=%u",
                                  static_cast<int>(state_), static_cast<int>(id), static_cast<unsigned>(size));
                    fail(message);
                }
                return;
            }
            if (state_ == State::Connecting)
            {
                return;
            }
            continue;
        }
        if (result != protocol::DecodeResult::NeedMore)
        {
            fail("Invalid packet framing or compression");
            return;
        }
        std::uint8_t buffer[16384];
        int received = transport_.receive(buffer, sizeof(buffer));
        if (received < 0)
        {
            auto message = transport_.error();
            fail(message.c_str());
            return;
        }
        if (received == 0)
        {
            break;
        }
        if (!decoder_.append(buffer, static_cast<std::size_t>(received)))
        {
            fail("Packet memory limit exceeded");
            return;
        }
    }
    if (sidebarDirty_ || labelsDirty_)
    {
        refreshSidebar();
    }
    if (labelsDirty_)
    {
        refreshLabels();
        labelsDirty_ = false;
    }
    if (state_ == State::Play && hasPosition_ && !loaded_)
    {
        const int x = static_cast<int>(std::floor(player_.x));
        const int y = static_cast<int>(std::floor(player_.y));
        const int z = static_cast<int>(std::floor(player_.z));
        BlockState state;
        const bool spawnReady = y < minSection_ * 16 || y >= (minSection_ + sectionCount_) * 16 ||
                                (world_.get(x, y, z, state) &&
                                 (y == minSection_ * 16 || world_.get(x, y - 1, z, state)));
        if (spawnReady || GetTime() >= loadDeadline_)
        {
            wire::Bytes none;
            if (sendPacket(44, none))
            {
                loaded_ = true;
            }
        }
    }
}

void Session::flush()
{
    transport_.update();
}

bool Session::sendSettings()
{
    wire::Bytes settings;
    wire::string(settings, "en_us");
    settings.push_back(static_cast<std::uint8_t>(viewChunks_));
    wire::var(settings, 0);
    settings.push_back(1);
    settings.push_back(0x7f);
    wire::var(settings, 1);
    settings.push_back(0);
    settings.push_back(0);
    wire::var(settings, 2);

    return sendPacket(state_ == State::Play ? 14 : 0, settings);
}

void Session::setViewDistance(int chunks)
{
#if defined(__PSP__)
    // Keep a ring of server chunks ahead of the local draw distance.
    chunks = std::clamp(chunks + 1, 3, 4);
#else
    chunks = std::clamp(chunks, 2, 8);
#endif
    if (viewChunks_ != chunks)
    {
        viewChunks_ = chunks;
        if (state_ == State::Play)
        {
            sendSettings();
        }
    }
}

// A transfer starts a new TCP stream; never retain borrowed decoder payloads.
bool Session::transfer(wire::Reader &reader)
{
    std::string host;
    std::int32_t port;
    if (!reader.str(host, 255) || host.empty() || !reader.var(port) || port < 1 || port > 65535 || reader.remaining())
    {
        return false;
    }
    transport_.close();
    resetPlayerInfo();
    inventory_ = {};
    clearPresentation();
    decoder_.reset();
    world_.clear();
    players_ = {};
    player_ = {};
    hasPosition_ = loaded_ = false;
    sequence_ = chatOffset_ = 0;
    chatSeen_ = 0;
    ++positionRevision_;
    compression_ = -1;
    transferring_ = true;
    dimensions_ = {};
    dimensionKey_.clear();
    ++worldRevision_;
    host_ = std::move(host);
    port_ = static_cast<std::uint16_t>(port);
    if (!transport_.begin(host_, port_))
    {
        fail(transport_.error().c_str());
        return false;
    }
    state_ = State::Connecting;
    deadline_ = GetTime() + 60;
    return true;
}

// Dispatches one packet for the current protocol state.
bool Session::handle(int id, const std::uint8_t *data, std::size_t size)
{
    wire::Reader r(data, size);
    if (state_ == State::Play && id == 134)
    {
        std::int32_t entity, count;
        if (!r.var(entity) || !r.var(count) || count < 0 || count > 128)
        {
            return false;
        }
        for (int i = 0; i < count; ++i)
        {
            std::int32_t attribute, modifiers;
            double base;
            if (!r.var(attribute) || !r.f64(base) || !std::isfinite(base) || !r.var(modifiers) || modifiers < 0 ||
                modifiers > 128)
            {
                return false;
            }
            double add = 0, multiply = 0, total = 1;
            for (int m = 0; m < modifiers; ++m)
            {
                std::string name;
                double amount;
                std::int32_t operation;
                if (!r.str(name, 256) || !r.f64(amount) || !std::isfinite(amount) || !r.var(operation) ||
                    operation < 0 || operation > 2)
                {
                    return false;
                }
                if (name == "minecraft:sprinting")
                {
                    continue;
                }
                if (operation == 0)
                {
                    add += amount;
                }
                else if (operation == 1)
                {
                    multiply += amount;
                }
                else
                {
                    total *= 1 + amount;
                }
            }
            if (entity == entityId_ && (attribute == 23 || attribute == 26))
            {
                const double speed = (base + add) * (1 + multiply) * total;
                if (std::isfinite(speed))
                {
                    if (attribute == 26)
                    {
                        player_.moveSpeed = static_cast<float>(std::clamp(speed, 0.0, 1.0));
                    }
                    else
                    {
                        inventory_.maxHealth = static_cast<float>(std::clamp(speed, 1.0, 1024.0));
                    }
                }
            }
        }
        return r.remaining() == 0;
    }
    const bool login = state_ == State::Login;
    const bool config = state_ == State::Configuration;
    if ((config && id == 12) || (state_ == State::Play && id == 132))
    {
        return transfer(r);
    }
    if ((login && id == 5) || (config && id == 0) || (state_ == State::Play && id == 21))
    {
        std::string key;
        if (!r.str(key, 256) || r.remaining())
        {
            return false;
        }
        wire::Bytes reply;
        wire::string(reply, key);
        const Cookie *found = nullptr;
        for (const auto &cookie : cookies_)
        {
            if (cookie.key == key)
            {
                found = &cookie;
                break;
            }
        }
        reply.push_back(found ? 1 : 0);
        if (found)
        {
            wire::var(reply, static_cast<int>(found->value.size()));
            wire::bytes(reply, found->value.data(), found->value.size());
        }
        return sendPacket(login ? 4 : config ? 1 : 21, reply);
    }
    if ((config && id == 11) || (state_ == State::Play && id == 122))
    {
        std::string key;
        std::int32_t count;
        if (!r.str(key, 256) || !r.var(count) || count < 0 || count > 5120 ||
            r.remaining() != static_cast<std::size_t>(count))
        {
            return false;
        }
        Cookie *target = nullptr;
        for (auto &cookie : cookies_)
        {
            if (cookie.key == key)
            {
                target = &cookie;
                break;
            }
        }
        if (!target)
        {
            target = &cookies_[nextCookie_++ % cookies_.size()];
        }
        target->key = std::move(key);
        target->value.reserve(5120);
        target->value.assign(r.data(), r.data() + count);
        return true;
    }
    if (state_ == State::Login)
    {
        if (id == 0)
        {
            std::string reason;
            if (r.str(reason, 262144))
            {
                error_ = reason;
            }
            else
            {
                error_ = "Server rejected login";
            }
            auto reasonCopy = error_;
            fail(reasonCopy.c_str());
            return false;
        }
        if (id == 1)
        {
            fail("Server requires authentication; offline-mode only");
            return false;
        }
        if (id == 3)
        {
            std::int32_t threshold;
            if (!r.var(threshold) || threshold < 0 || !empty(r))
            {
                return false;
            }
            compression_ = threshold;
            decoder_.setCompression(threshold);
            return true;
        }
        if (id == 4)
        {
            std::int32_t message;
            if (!r.var(message))
            {
                return false;
            }
            wire::Bytes reply;
            wire::var(reply, message);
            reply.push_back(0);
            return sendPacket(2, reply);
        }
        if (id == 2)
        {
            wire::Bytes none;
            if (!sendPacket(3, none))
            {
                return false;
            }
            state_ = State::Configuration;
            return sendSettings();
        }
        return false;
    }
    if (state_ == State::Configuration)
    {
        if (id == 7)
        {
            std::string registry;
            if (!r.str(registry, 256))
            {
                return false;
            }
            if (registry != "minecraft:dimension_type")
            {
                return true;
            }
            std::int32_t count;
            if (!r.var(count) || count < 0 || count > static_cast<int>(dimensions_.size()))
            {
                return false;
            }
            dimensions_ = {};
            for (int i = 0; i < count; ++i)
            {
                std::string name;
                std::uint8_t present;
                if (!r.str(name, 256) || !r.byte(present) || present > 1)
                {
                    return false;
                }
                int minY = 0;
                int height = 0;
                unsigned found = 0;
                if (present)
                {
                    std::uint8_t tag;
                    if (!r.byte(tag) || tag != 10 || !dimensionValue(r, tag, 0, minY, height, found))
                    {
                        return false;
                    }
                }
                if (found == 3 && minY >= -4096 && minY <= 4096 && !(minY % 16) && height > 0 && height <= 1024 &&
                    !(height % 16))
                {
                    dimensions_[i] = {minY / 16, height / 16};
                }
            }
            return r.remaining() == 0;
        }
        if (id == 2)
        {
            std::string reason;
            readChatText(r, reason);
            reason = displayText(reason, 512);
            fail(reason.empty() ? "Server disconnected during configuration" : reason.c_str());
            return false;
        }
        if (id == 3)
        {
            wire::Bytes none;
            if (!sendPacket(3, none))
            {
                return false;
            }
            state_ = State::Play;
            return true;
        }
        if (id == 4 || id == 5)
        {
            wire::Bytes response;
            wire::bytes(response, data, size);
            return sendPacket(id == 4 ? 4 : 5, response);
        }
        if (id == 15)
        {
            // No server registry packs are cached across connections.
            wire::Bytes response;
            wire::var(response, 0);
            return sendPacket(7, response);
        }
        if (id == 9)
        {
            if (size < 16 || !r.skip(16))
            {
                return false;
            }
            std::string url, hash;
            std::uint8_t required;
            if (!r.str(url, 32767) || !r.str(hash, 40) || !r.byte(required) || required > 1)
            {
                return false;
            }
            if (required)
            {
                fail("Server requires a resource pack");
                return false;
            }
            wire::Bytes response;
            wire::bytes(response, data, 16);
            wire::var(response, 1);
            return sendPacket(6, response);
        }
        if (id == 20)
        {
            fail("Server requires code of conduct confirmation");
            return false;
        }
        return true;
    }
    if (state_ == State::Play)
    {
        return handlePlay(id, data, size);
    }
    return false;
}

// Reads a dimension selection packet and resets world state.
bool Session::selectDimension(wire::Reader &r, bool respawn)
{
    std::int32_t dimensionType;
    std::string dimension;
    std::uint64_t seed;
    std::int32_t gameMode;
    if (!r.var(dimensionType) || dimensionType < 0 || !r.str(dimension, 256) || !r.u64(seed) ||
        !r.var(gameMode) || gameMode < 0 || gameMode > 3)
    {
        return false;
    }
    const int previousMin = minSection_;
    const int previousCount = sectionCount_;
    if (dimensionType < static_cast<int>(dimensions_.size()) && dimensions_[dimensionType].sections)
    {
        minSection_ = dimensions_[dimensionType].minSection;
        sectionCount_ = dimensions_[dimensionType].sections;
    }
    else if (dimension == "minecraft:overworld")
    {
        minSection_ = -4;
        sectionCount_ = 24;
    }
    else if (dimension == "minecraft:the_nether" || dimension == "minecraft:the_end")
    {
        minSection_ = 0;
        sectionCount_ = 16;
    }
    else
    {
        fail("Missing or unsupported dimension bounds in server registry");
        return false;
    }
    if (!respawn || dimension != dimensionKey_ || seed != dimensionSeed_ || minSection_ != previousMin ||
        sectionCount_ != previousCount)
    {
        world_.clear();
        players_ = {};
        drops_ = {};
        ++worldRevision_;
    }
    dimensionKey_ = std::move(dimension);
    dimensionSeed_ = seed;
    setGameMode(gameMode);
    dead_ = respawnRequested_ = false;
    deathMessage_.clear();
    cameraEntity_ = -1;
    handStart_ = -1;
    inventory_.open = false;
    inventory_.container = inventory_.rows = 0;
    inventory_.carried = {};
    hasPosition_ = loaded_ = false;
    player_.onGround = false;
    player_.input = 0;
    player_.vx = player_.vy = player_.vz = 0;
    sentSprint_ = false;
    sentInput_ = 0xff;
    ++positionRevision_;
    return true;
}

// Finds an active remote player by entity id.
RemotePlayer *Session::findPlayer(int id)
{
    for (auto &p : players_)
    {
        if (p.active && p.id == id)
        {
            return &p;
        }
    }
    for (auto &drop : drops_)
    {
        if (drop.active && drop.id == id)
        {
            return &drop;
        }
    }
    return nullptr;
}

// Handles one packet in the play state.
bool Session::handlePlay(int id, const std::uint8_t *data, std::size_t size)
{
    wire::Reader r(data, size);
    switch (id)
    {
    case 14:
    case 25:
    case 34:
    case 39:
    case 43:
    case 69:
    case 89:
    case 95:
    case 114:
    case 116:
    case 117:
    case 123:
    case 127:
        readEffects(id, r);
        return true;
    case 104:
        readEquipment(r);
        return true;
    case 17:
    case 18:
    case 20:
    case 60:
    case 98:
    case 105:
    case 106:
    case 107:
    case 110:
        // A rejected inventory frame must not desynchronize the play stream.
        readInventory(id, r);
        return true;
    default:
        break;
    }
    if (id == 103)
    {
        std::int32_t entity;
        if (!r.var(entity))
        {
            return false;
        }
        if (entity != entityId_)
        {
            return true;
        }
        std::uint8_t low, middle;
        std::int32_t high, extension = 0;
        if (!r.byte(low))
        {
            return false;
        }
        if (!low)
        {
            player_.vx = player_.vy = player_.vz = 0;
            return r.remaining() == 0;
        }
        if (!r.byte(middle) || !r.i32(high) || ((low & 4) && (!r.var(extension) || extension < 0)))
        {
            return false;
        }
        const auto scale = std::uint64_t(low & 3) + std::uint64_t(extension) * 4;
        if (scale > 1024)
        {
            return false;
        }
        const auto packed =
            (std::uint64_t(static_cast<std::uint32_t>(high)) << 16) | (std::uint64_t(middle) << 8) | low;
        const auto component = [&](int shift)
        {
            const auto value = std::min<std::uint64_t>((packed >> shift) & 0x7fff, 32766);
            return (static_cast<float>(value) * (2.0f / 32766) - 1) * static_cast<float>(scale);
        };
        player_.vx = component(3);
        player_.vy = component(18);
        player_.vz = component(33);
        return r.remaining() == 0;
    }
    if (id == 71)
    {
        // Profile updates only affect presentation. The decoder has already
        // consumed the frame, so an unsupported profile can be left out safely.
        handlePlayers(id, r);
        return true;
    }
    if (id == 2 || id == 70 || id == 85 || id == 111)
    {
        return handlePlayers(id, r);
    }
    if (id == 80 || id == 100 || id == 108 || id == 112)
    {
        readScoreboard(id, r);
        return true;
    }
    if (id == 66 || id == 124 || id == 33)
    {
        if (id == 66)
        {
            wire::Reader signature(data, size);
            std::int32_t index;
            std::uint8_t present;
            if (!signature.var(index) || !signature.skip(16) || !signature.var(index) || !signature.byte(present) ||
                present > 1)
            {
                return false;
            }
            if (present)
            {
                chatSeen_ = (chatSeen_ >> 1) | (1u << 19);
                ++chatOffset_;
                if (chatOffset_ >= 64)
                {
                    wire::Bytes ack;
                    wire::var(ack, chatOffset_);
                    if (!sendPacket(6, ack))
                    {
                        return false;
                    }
                    chatOffset_ = 0;
                }
            }
        }
        std::string text;
        bool valid = id == 66 ? readPlayerChat(r, text) : id == 124 ? readRichChatText(r, text) : readChatText(r, text);
        if (valid && id == 124)
        {
            std::uint8_t overlay;
            if (r.byte(overlay) && overlay)
            {
                actionBar_ = std::move(text);
                actionBarEnd_ = GetTime() + 2.5;
                return true;
            }
        }
        if (valid && !text.empty())
        {
            for (std::size_t i = 1; i < chat_.size(); ++i)
            {
                chat_[i - 1] = std::move(chat_[i]);
            }
            chat_.back() = id == 124 ? std::move(text) : displayText(text, 512);
        }
        return true;
    }
    if (id == 32)
    {
        std::string reason;
        readChatText(r, reason);
        reason = displayText(reason, 512);
        fail(reason.empty() ? "Server disconnected during play" : reason.c_str());
        return false;
    }
    if (id == cbKeep)
    {
        if (size != 8)
        {
            return false;
        }
        wire::Bytes reply;
        wire::bytes(reply, data, size);
        return sendPacket(28, reply);
    }
    if (id == 11)
    {
        std::int32_t batchSize;
        if (!r.var(batchSize) || batchSize < 0 || r.remaining())
        {
            return false;
        }
        wire::Bytes reply;
#if defined(__PSP__)
        wire::f32(reply, 3.0f);
#else
        wire::f32(reply, 8.0f);
#endif
        return sendPacket(11, reply);
    }
    if (id == 96)
    {
        std::int32_t x, z;
        if (!r.var(x) || !r.var(z) || r.remaining())
        {
            return false;
        }
        world_.setServerCenter(x, z);
        return true;
    }
    if (id == cbLogin)
    {
        std::int32_t entity, worlds, temp;
        std::uint8_t b;
        std::string str;
        if (!r.i32(entity) || !r.byte(b) || !r.var(worlds) || worlds < 0 || worlds > 256)
        {
            return false;
        }
        for (int i = 0; i < worlds; ++i)
        {
            if (!r.str(str, 256))
            {
                return false;
            }
        }
        for (int i = 0; i < 3; ++i)
        {
            if (!r.var(temp) || temp < 0)
            {
                return false;
            }
        }
        if (!r.skip(1) || !r.byte(b) || !r.skip(1))
        {
            return false;
        }
        entityId_ = entity;
        immediateRespawn_ = b == 0;
        return selectDimension(r, false);
    }
    if (id == 84)
    {
        return selectDimension(r, true);
    }
    if (id == cbPos)
    {
        std::int32_t tp, flags;
        PlayerPose next;
        next.moveSpeed = player_.moveSpeed;
        next.input = player_.input;
        double vx, vy, vz;
        if (!r.var(tp) || !r.f64(next.x) || !r.f64(next.y) || !r.f64(next.z) || !r.f64(vx) || !r.f64(vy) ||
            !r.f64(vz) || !r.f32(next.yaw) || !r.f32(next.pitch) || !r.i32(flags))
        {
            return false;
        }
        if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z) || !std::isfinite(next.yaw) ||
            !std::isfinite(next.pitch) || !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz))
        {
            return false;
        }
        if (flags & 1)
        {
            next.x += player_.x;
        }
        if (flags & 2)
        {
            next.y += player_.y;
        }
        if (flags & 4)
        {
            next.z += player_.z;
        }
        if (flags & 8)
        {
            next.yaw += player_.yaw;
        }
        if (flags & 16)
        {
            next.pitch += player_.pitch;
        }
        if (std::abs(next.x) > 30000000 || std::abs(next.z) > 30000000 || std::abs(next.y) > 4096)
        {
            return false;
        }
        float oldX = player_.vx;
        float oldY = player_.vy;
        float oldZ = player_.vz;
        if (flags & 256)
        {
            constexpr float radians = 3.14159265358979323846f / 180;
            const float pitch = (player_.pitch - next.pitch) * radians;
            const float yaw = (player_.yaw - next.yaw) * radians;
            const float rotatedY = oldY * std::cos(pitch) + oldZ * std::sin(pitch);
            oldZ = oldZ * std::cos(pitch) - oldY * std::sin(pitch);
            oldY = rotatedY;
            const float rotatedX = oldX * std::cos(yaw) + oldZ * std::sin(yaw);
            oldZ = oldZ * std::cos(yaw) - oldX * std::sin(yaw);
            oldX = rotatedX;
        }
        next.vx = static_cast<float>(vx) + ((flags & 32) ? oldX : 0);
        next.vy = static_cast<float>(vy) + ((flags & 64) ? oldY : 0);
        next.vz = static_cast<float>(vz) + ((flags & 128) ? oldZ : 0);
        player_ = next;
        if (!hasPosition_)
        {
            // A proxy can wait for this acknowledgement before sending the
            // rest of an empty or deferred spawn area.
            loadDeadline_ = GetTime() + 3.0;
        }
        hasPosition_ = true;
        ++positionRevision_;
        world_.setCenter(static_cast<int>(std::floor(next.x)), static_cast<int>(std::floor(next.y)),
                         static_cast<int>(std::floor(next.z)));
        wire::Bytes response;
        wire::var(response, tp);
        wire::f64(response, next.x);
        wire::f64(response, next.y);
        wire::f64(response, next.z);
        wire::f32(response, next.yaw);
        wire::f32(response, next.pitch);
        if (!sendPacket(0, response))
        {
            return false;
        }
        sendMovement(false);
        return true;
    }
    if (id == cbChunk)
    {
        return chunks_.decode(world_, data, size, minSection_, sectionCount_);
    }
    if (id == 38)
    {
        std::int32_t z, x;
        if (!r.i32(z) || !r.i32(x))
        {
            return false;
        }
        world_.eraseColumn(x, z);
        return true;
    }
    if (id == 8)
    {
        std::uint64_t pos;
        std::int32_t state;
        if (!r.u64(pos) || !r.var(state) || state < 0 || state > 65535)
        {
            return false;
        }
        world_.set(signedBits(pos >> 38, 26), signedBits(pos & 0xfff, 12), signedBits((pos >> 12) & 0x3ffffff, 26),
                   static_cast<BlockState>(state));
        return true;
    }
    if (id == 86)
    {
        std::uint64_t packed;
        std::int32_t count;
        if (!r.u64(packed) || !r.var(count) || count < 0 || count > 4096)
        {
            return false;
        }
        const int sx = signedBits(packed >> 42, 22), sz = signedBits((packed >> 20) & 0x3fffff, 22),
                  sy = signedBits(packed & 0xfffff, 20);
        if (std::abs(std::int64_t(sx)) > 1875001 || std::abs(std::int64_t(sz)) > 1875001 ||
            std::abs(std::int64_t(sy)) > 2048)
        {
            return false;
        }
        for (int i = 0; i < count; ++i)
        {
            std::uint64_t v;
            if (!r.varlong(v) || (v >> 12) > 65535)
            {
                return false;
            }
            world_.set(sx * 16 + static_cast<int>((v >> 8) & 15), sy * 16 + static_cast<int>(v & 15),
                       sz * 16 + static_cast<int>((v >> 4) & 15), static_cast<BlockState>(v >> 12));
        }
        return true;
    }
    if (id == 1)
    {
        std::int32_t entity, type;
        double x, y, z;
        std::array<std::uint8_t, 16> uuid{};
        if (!r.var(entity) || r.remaining() < uuid.size())
        {
            return false;
        }
        std::copy_n(r.data(), uuid.size(), uuid.begin());
        if (!r.skip(uuid.size()) || !r.var(type) || !r.f64(x) || !r.f64(y) || !r.f64(z))
        {
            return false;
        }
        std::uint8_t velocity, pitch, yaw, head;
        std::int32_t extra;
        if (!r.byte(velocity) || (velocity && !r.skip(5)) || ((velocity & 4) && !r.var(extra)) || !r.byte(pitch) ||
            !r.byte(yaw) || !r.byte(head) || !r.var(extra))
        {
            return false;
        }
        if (type == remoteType::Item && std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
        {
            auto drop = std::find_if(drops_.begin(), drops_.end(), [=](const RemotePlayer &p)
                                     { return p.active && p.id == entity; });
            if (drop == drops_.end())
            {
                drop = std::find_if(drops_.begin(), drops_.end(), [](const RemotePlayer &p) { return !p.active; });
            }
            if (drop == drops_.end())
            {
                const auto distance = [&](double px, double py, double pz)
                {
                    const double dx = px - player_.x, dy = py - player_.y, dz = pz - player_.z;
                    return dx * dx + dy * dy + dz * dz;
                };
                drop = std::max_element(drops_.begin(), drops_.end(), [&](const RemotePlayer &a, const RemotePlayer &b)
                                        { return distance(a.x, a.y, a.z) < distance(b.x, b.y, b.z); });
                if (distance(x, y, z) >= distance(drop->x, drop->y, drop->z))
                {
                    return true;
                }
            }
            *drop = {};
            drop->active = true;
            drop->id = entity;
            drop->type = type;
            drop->x = x;
            drop->y = y;
            drop->z = z;
            return true;
        }
        if ((type == remoteType::Player || type == remoteType::ArmorStand || type == remoteType::Villager ||
             type == remoteType::ZombieVillager || type == remoteType::WanderingTrader ||
             type == remoteType::Mannequin || type == remoteType::Interaction || type == remoteType::TextDisplay) &&
            std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
        {
            RemotePlayer *p = findPlayer(entity);
            if (!p)
            {
                for (auto &candidate : players_)
                {
                    if (!candidate.active)
                    {
                        p = &candidate;
                        break;
                    }
                }
            }
            if (!p)
            {
                const float dx = static_cast<float>(x - player_.x);
                const float dy = static_cast<float>(y - player_.y);
                const float dz = static_cast<float>(z - player_.z);
                float farthest = dx * dx + dy * dy + dz * dz;
                for (auto &candidate : players_)
                {
                    const float cx = static_cast<float>(candidate.x - player_.x);
                    const float cy = static_cast<float>(candidate.y - player_.y);
                    const float cz = static_cast<float>(candidate.z - player_.z);
                    const float distance = cx * cx + cy * cy + cz * cz;
                    if (distance > farthest)
                    {
                        p = &candidate;
                        farthest = distance;
                    }
                }
            }
            if (p)
            {
                *p = {};
                p->active = true;
                p->id = entity;
                p->type = type;
                if (type == remoteType::Interaction)
                {
                    p->width = p->height = 1;
                }
                p->uuid = uuid;
                p->x = x;
                p->y = y;
                p->z = z;
                p->yaw = yaw * 360.0f / 256;
                p->pitch = pitch * 360.0f / 256;
                p->headYaw = head * 360.0f / 256;
                syncPlayer(*p);
            }
        }
        return true;
    }
    if (id == 101)
    {
        // Presentation data is framed separately from movement and interaction.
        readEntityMetadata(r);
        return true;
    }
    if (id == 54 || id == 55)
    {
        std::int32_t entity, flags;
        if (!r.var(entity) || !r.var(flags) || flags < 0)
        {
            return false;
        }
        std::int64_t sums[3] = {};
        int count = flags >> 1;
        if (count > 1024)
        {
            return false;
        }
        for (int i = 0; i < std::max(count, 1); ++i)
        {
            std::int32_t ticks;
            if (count && (!r.var(ticks) || ticks < 0))
            {
                return false;
            }
            for (auto &sum : sums)
            {
                std::uint16_t value;
                if (!r.u16(value))
                {
                    return false;
                }
                sum += static_cast<std::int16_t>(value);
            }
        }
        auto *p = findPlayer(entity);
        if (p)
        {
            p->x += sums[0] / 4096.0;
            p->y += sums[1] / 4096.0;
            p->z += sums[2] / 4096.0;
        }
        if (id == 55)
        {
            std::uint8_t yaw, pitch;
            if (!r.byte(yaw) || !r.byte(pitch))
            {
                return false;
            }
            if (p)
            {
                p->yaw = yaw * 360.0f / 256;
                p->pitch = pitch * 360.0f / 256;
            }
        }
        return true;
    }
    if (id == 78)
    {
        std::int32_t n;
        if (!r.var(n) || n < 0 || n > 4096)
        {
            return false;
        }
        for (int i = 0; i < n; ++i)
        {
            std::int32_t entity;
            if (!r.var(entity))
            {
                return false;
            }
            auto *p = findPlayer(entity);
            if (p)
            {
                p->active = false;
            }
        }
        return true;
    }
    if (id == 57)
    {
        std::int32_t entity;
        std::uint8_t grounded, yaw, pitch;
        if (!r.var(entity) || !r.byte(grounded) || !r.byte(yaw) || !r.byte(pitch))
        {
            return false;
        }
        if (auto *p = findPlayer(entity))
        {
            p->yaw = yaw * 360.0f / 256;
            p->pitch = pitch * 360.0f / 256;
        }
        return true;
    }
    if (id == 35)
    {
        std::int32_t entity, kind;
        double x = 0, y = 0, z = 0;
        float yaw, pitch;
        std::uint8_t grounded;
        if (!r.var(entity) || !r.var(kind) || kind < 0 || kind > 1)
        {
            return false;
        }
        if (kind == 0)
        {
            if (!r.f64(x) || !r.f64(y) || !r.f64(z))
            {
                return false;
            }
        }
        else
        {
            std::int32_t count;
            if (!r.var(count) || count < 1 || count > 64)
            {
                return false;
            }
            for (int i = 0; i < count; ++i)
            {
                std::int32_t ticks;
                if (!r.f64(x) || !r.f64(y) || !r.f64(z) || !r.var(ticks))
                {
                    return false;
                }
            }
        }
        if (!r.f32(yaw) || !r.f32(pitch) || !r.byte(grounded) || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(z))
        {
            return false;
        }
        if (auto *p = findPlayer(entity))
        {
            p->x = x;
            p->y = y;
            p->z = z;
            p->yaw = yaw;
            p->pitch = pitch;
        }
        return true;
    }
    if (id == 62)
    {
        if (size != 4)
        {
            return false;
        }
        wire::Bytes body;
        wire::bytes(body, data, size);
        return sendPacket(45, body);
    }
    if (id == 82)
    {
        if (size < 16 || !r.skip(16))
        {
            return false;
        }
        std::string url, hash;
        std::uint8_t required;
        if (!r.str(url, 32767) || !r.str(hash, 40) || !r.byte(required) || required > 1)
        {
            return false;
        }
        if (required)
        {
            fail("Server requires a resource pack");
            return false;
        }
        wire::Bytes reply;
        wire::bytes(reply, data, 16);
        wire::var(reply, 1);
        return sendPacket(50, reply);
    }
    if (id == 120)
    {
        wire::Bytes body;
        if (!sendPacket(16, body))
        {
            return false;
        }
        world_.clear();
        dimensionKey_.clear();
        inventory_ = {};
        clearPresentation();
        players_ = {};
        teams_ = {};
        members_ = {};
        labelsDirty_ = false;
        hasPosition_ = loaded_ = false;
        chatOffset_ = 0;
        chatSeen_ = 0;
        ++positionRevision_;
        state_ = State::Configuration;
        ++worldRevision_;
        deadline_ = GetTime() + 30.0;
        return sendSettings();
    }
    if (id == 128)
    {
        std::int32_t entity, flags;
        double x, y, z, ignored;
        float yaw, pitch;
        if (!r.var(entity) || !r.f64(x) || !r.f64(y) || !r.f64(z) || !r.f64(ignored) || !r.f64(ignored) ||
            !r.f64(ignored) || !r.f32(yaw) || !r.f32(pitch) || !r.i32(flags))
        {
            return false;
        }
        if (auto *p = findPlayer(entity))
        {
            p->x = (flags & 1) ? p->x + x : x;
            p->y = (flags & 2) ? p->y + y : y;
            p->z = (flags & 4) ? p->z + z : z;
            p->yaw = yaw;
            p->pitch = pitch;
        }
        return true;
    }
    return true;
}

// Sends a chat message or command.
bool Session::sendChat(const std::string &text)
{
    if (state_ != State::Play || text.empty() || text.size() > 1024)
    {
        return false;
    }
    wire::Bytes body;
    if (text[0] == '/')
    {
        if (text.size() == 1)
        {
            return false;
        }
        wire::string(body, text.substr(1));
        return sendPacket(7, body);
    }
    wire::string(body, text);
    wire::u64(body, static_cast<std::uint64_t>(std::time(nullptr)) * 1000);
    wire::u64(body, 0);
    body.push_back(0);
    wire::var(body, chatOffset_);
    body.push_back(static_cast<std::uint8_t>(chatSeen_));
    body.push_back(static_cast<std::uint8_t>(chatSeen_ >> 8));
    body.push_back(static_cast<std::uint8_t>(chatSeen_ >> 16));
    body.push_back(0); // The protocol permits zero to omit the optional checksum.
    if (!sendPacket(9, body))
    {
        return false;
    }
    chatOffset_ = 0;
    return true;
}

// Updates the local player pose and world center.
void Session::setPose(const PlayerPose &pose)
{
    if (state_ != State::Play || !hasPosition_)
    {
        return;
    }
    player_ = pose;
    world_.setCenter(static_cast<int>(std::floor(pose.x)), static_cast<int>(std::floor(pose.y)),
                     static_cast<int>(std::floor(pose.z)));
}

// Sends the periodic player movement packet.
void Session::movementTick(const PlayerPose &pose)
{
    if (state_ != State::Play || !hasPosition_ || !loaded_ || dead_)
    {
        return;
    }
    setPose(pose);
    sendMovement();
}

void Session::sendMovement(bool tickEnd)
{
    const auto &pose = player_;
    wire::Bytes body;
    body.reserve(48);
    const bool sprint = (pose.input & 64) != 0;
    if (sprint != sentSprint_)
    {
        wire::var(body, entityId_);
        wire::var(body, sprint ? 1 : 2);
        wire::var(body, 0);
        if (!sendPacket(42, body))
        {
            return;
        }
        sentSprint_ = sprint;
        body.clear();
    }
    if (pose.input != sentInput_)
    {
        body.push_back(pose.input);
        if (!sendPacket(43, body))
        {
            return;
        }
        sentInput_ = pose.input;
        body.clear();
    }
    wire::f64(body, pose.x);
    wire::f64(body, pose.y);
    wire::f64(body, pose.z);
    wire::f32(body, pose.yaw);
    wire::f32(body, pose.pitch);
    body.push_back((pose.onGround ? 1 : 0) | (pose.horizontalCollision ? 2 : 0));
    if (!sendPacket(31, body))
    {
        return;
    }
    if (tickEnd)
    {
        wire::Bytes none;
        sendPacket(13, none);
    }
}

// Sends a block dig/break action.
void Session::blockAction(int status, int x, int y, int z, int face)
{
    if (state_ != State::Play || face < 0 || face > 5 || (status != 0 && status != 2 && status != 3))
    {
        return;
    }
    wire::Bytes body;
    wire::var(body, status);
    wire::u64(body, wire::blockPosition(x, y, z));
    body.push_back(static_cast<std::uint8_t>(face));
    wire::var(body, sequence_++);
    if (sendPacket(41, body) && status == 0)
    {
        swingHand();
    }
}

// Protocol 777 separates entity attacks from interaction packets.
void Session::attackEntity(int entity)
{
    if (state_ != State::Play || entity < 0)
    {
        return;
    }
    wire::Bytes body;
    wire::var(body, entity);
    if (sendPacket(1, body))
    {
        swingHand();
    }
}

void Session::interactEntity(int entity, bool sneak)
{
    if (state_ != State::Play || entity < 0)
    {
        return;
    }
    wire::Bytes body;
    wire::var(body, entity);
    wire::var(body, 0);
    body.push_back(0); // LP_VEC3 zero: relative hit location at the entity's feet.
    body.push_back(sneak ? 1 : 0);
    sendPacket(26, body);
}

// Drops the item held in the selected hotbar slot.
void Session::dropItem(bool stack)
{
    if (state_ != State::Play)
    {
        return;
    }
    wire::Bytes body;
    wire::var(body, stack ? 4 : 5);
    wire::u64(body, 0);
    body.push_back(0);
    wire::var(body, sequence_++);
    if (sendPacket(41, body))
    {
        auto &held = inventory_.player[Inventory::HotbarStart + inventory_.selected];
        if (stack || held.count <= 1)
        {
            held = {};
        }
        else
        {
            --held.count;
        }
    }
}

// Sends a block placement action.
void Session::placeBlock(int x, int y, int z, int face, float hx, float hy, float hz)
{
    if (state_ != State::Play || face < 0 || face > 5)
    {
        return;
    }
    wire::Bytes body;
    wire::var(body, 0);
    wire::u64(body, wire::blockPosition(x, y, z));
    wire::var(body, face);
    wire::f32(body, hx);
    wire::f32(body, hy);
    wire::f32(body, hz);
    body.push_back(0);
    body.push_back(0);
    wire::var(body, sequence_++);
    if (sendPacket(66, body))
    {
        swingHand();
    }
}
} // namespace craft
