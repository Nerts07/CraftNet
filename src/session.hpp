#pragma once
#include "chunk.hpp"
#include "inventory.hpp"
#include "net.hpp"
#include "protocol.hpp"
#include "wire.hpp"
#include <array>
#include <string>
#include <vector>

namespace craft
{
struct PlayerPose
{
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    float vx = 0, vy = 0, vz = 0;
    float moveSpeed = 0.1f;
    std::uint8_t input = 0;
    bool horizontalCollision = false;
    bool onGround = false;
};

struct Sidebar
{
    struct Row
    {
        std::string label, value;
    };
    std::string title;
    std::array<Row, 15> rows;
    int count = 0;
    bool visible = false;
};

namespace remoteType
{
constexpr int ArmorStand = 5, Interaction = 70, Item = 72, Mannequin = 84, TextDisplay = 135, Villager = 143,
              WanderingTrader = 145, ZombieVillager = 157, Player = 159;
}
constexpr std::size_t RemoteEntityLimit = 96;

struct RemotePlayer
{
    int id = 0;
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    bool active = false;
    int type = remoteType::Player;
    std::array<char, 96> name{};
    std::array<char, 96> description{};
    std::array<std::uint8_t, 16> uuid{};
    std::array<char, 17> account{};
    std::array<char, 65> skin{};
    std::array<char, 256> label{};
    float headYaw = 0;
    std::array<float, 3> labelOffset{};
    float labelScale = 1;
    float width = 0.6f, height = 1.8f;
    unsigned swing = 0;
    float swingSeconds = 0.3f;
    double hurtUntil = 0;
    std::array<int, 6> equipment{}; // Main hand, boots, leggings, chest, helmet, offhand
    std::array<int, 6> armorDyes{-1, -1, -1, -1, -1, -1};
    int itemCount = 0;
    bool offhandSwing = false;
    bool slim = false;
    bool invisible = false;
    bool sneaking = false;
    bool hideLabel = false;
    bool spectator = false;
    bool marker = false;
    bool small = false;
    bool customNameVisible = false;
    std::uint8_t skinParts = 127;
};

class Session final
{
  public:
    enum class State
    {
        Idle,
        Connecting,
        Login,
        Configuration,
        Play,
        Failed
    };
    // Starts a connection and offline-mode login.
    bool connect(const std::string &host, std::uint16_t port, const std::string &username);
    // Processes network input and server packets.
    void update();
    // Closes the connection and resets session state.
    void disconnect();
    // Returns the current connection state.
    State state() const
    {
        return state_;
    }
    // Returns the last session error message.
    const std::string &error() const
    {
        return error_;
    }
    // Returns the remote world.
    const World &world() const
    {
        return world_;
    }
    // Returns the local player pose.
    const PlayerPose &player() const
    {
        return player_;
    }
    // Returns the remote player table.
    const std::array<RemotePlayer, RemoteEntityLimit> &players() const
    {
        return players_;
    }
    // Returns the recent chat history.
    const std::array<std::string, 8> &chat() const
    {
        return chat_;
    }
    // Returns whether a player position packet has been received.
    bool hasPosition() const
    {
        return hasPosition_ && loaded_;
    }
    // Returns the revision that changes when the server sends a new position.
    std::uint64_t positionRevision() const
    {
        return positionRevision_;
    }
    std::uint64_t worldRevision() const
    {
        return worldRevision_;
    }
    // Returns the total buffered byte count.
    std::size_t bufferBytes() const
    {
        return decoder_.allocatedBytes() + transport_.bufferedBytes();
    }
    // Updates the local player pose and world center.
    void setPose(const PlayerPose &pose);
    void movementTick(const PlayerPose &pose);
    void flush();
    const Inventory &inventory() const { return inventory_; }
    const Sidebar &sidebar() const { return sidebar_; }
    unsigned rosterRevision() const { return rosterRevision_; }
    std::vector<std::string> tabPlayers() const;
    void setViewDistance(int chunks);
    const std::array<RemotePlayer, 64> &drops() const { return drops_; }
    bool dead() const { return dead_; }
    bool spectator() const { return gameMode_ == 3; }
    bool respawnRequested() const { return respawnRequested_; }
    const std::string &deathMessage() const { return deathMessage_; }
    const std::string &title() const { return title_; }
    const std::string &subtitle() const { return subtitle_; }
    const std::string &actionBar() const { return actionBar_; }
    float titleAlpha() const;
    bool actionBarVisible() const;
    float handSwing() const;
    const RemotePlayer *cameraTarget() const;
    void requestRespawn();
    void swingHand(bool punch = false);
    void openInventory();
    void closeInventory();
    void clickInventory(int slot, int button);
    void selectHotbar(int slot);
    void useItem();
    // Sends a chat message or command.
    bool sendChat(const std::string &text);
    // Sends a block dig/break action.
    void blockAction(int status, int x, int y, int z, int face);
    // Drops one selected item, or the entire stack when requested.
    void dropItem(bool stack = false);
    void attackEntity(int entity);
    void interactEntity(int entity, bool sneak);
    // Sends a block placement action.
    void placeBlock(int x, int y, int z, int face, float hx, float hy, float hz);

  private:
    // Frames and sends one protocol packet.
    bool sendPacket(int id, const wire::Bytes &body);
    bool sendSettings();
    bool transfer(wire::Reader &reader);
    // Dispatches one packet for the current protocol state.
    bool handle(int id, const std::uint8_t *data, std::size_t size);
    // Handles one packet in the play state.
    bool handlePlay(int id, const std::uint8_t *data, std::size_t size);
    bool readEffects(int id, wire::Reader &reader);
    void readScoreboard(int id, wire::Reader &reader);
    void refreshSidebar();
    bool readEquipment(wire::Reader &reader);
    void clearPresentation();
    void setDead();
    void setGameMode(int mode);
    bool readInventory(int id, wire::Reader &reader);
    // Reads a dimension selection packet and resets world state.
    bool selectDimension(wire::Reader &r, bool respawn);
    // Sends the periodic player movement packet.
    void sendMovement(bool tickEnd = true);
    // Records a fatal session error.
    void fail(const char *message);
    // Finds an active remote player by entity id.
    RemotePlayer *findPlayer(int id);
    bool handlePlayers(int id, wire::Reader &reader);
    bool readEntityMetadata(wire::Reader &reader);
    void syncPlayer(RemotePlayer &player);
    void refreshLabels();
    void resetPlayerInfo();
    struct Profile
    {
        std::array<std::uint8_t, 16> uuid{};
        std::string account, skin, display;
        bool used = false, slim = false, spectator = false, listed = false, hasDisplay = false;
        int listOrder = 0;
        unsigned age = 0;
    };
    struct Team
    {
        std::string id, prefix, suffix;
        int color = 15, visibility = 0;
        bool used = false;
    };
    struct Membership
    {
        std::string account;
        int team = -1;
    };
    std::vector<Profile> profiles_;
    std::array<Team, 128> teams_{};
    std::array<Membership, 512> members_{};
    unsigned profileAge_ = 0;
    unsigned rosterRevision_ = 0;
    int viewChunks_ = 2;
    bool labelsDirty_ = false;
    struct ScoreFormat
    {
        bool read(wire::Reader &reader);
        int type = -1;
        std::string text;
    };
    struct ScoreObjective
    {
        std::string name, title;
        ScoreFormat format;
    };
    struct ScoreEntry
    {
        std::string owner, objective, display;
        std::int32_t value = 0;
        bool customDisplay = false;
        ScoreFormat format;
    };
    std::vector<ScoreObjective> objectives_;
    std::vector<ScoreEntry> scores_;
    std::array<std::string, 19> scoreSlots_{};
    Sidebar sidebar_{};
    bool sidebarDirty_ = false;
    Transport transport_;
    protocol::Decoder decoder_;
    World world_;
    ChunkDecoder chunks_;
    std::array<RemotePlayer, RemoteEntityLimit> players_{};
    std::array<RemotePlayer, 64> drops_{};
    struct Dimension
    {
        int minSection = 0;
        int sections = 0;
    };
    std::array<Dimension, 64> dimensions_{};
    std::array<std::string, 8> chat_{};
    struct Cookie
    {
        std::string key;
        wire::Bytes value;
    };
    std::array<Cookie, 8> cookies_{};
    unsigned nextCookie_ = 0;
    bool transferring_ = false;
    PlayerPose player_{};
    Inventory inventory_{};
    int gameMode_ = 0, cameraEntity_ = -1;
    bool dead_ = false, respawnRequested_ = false, immediateRespawn_ = false;
    std::string deathMessage_, title_, subtitle_, actionBar_;
    std::array<int, 3> titleTicks_{10, 70, 20};
    double titleStart_ = 0, titleEnd_ = 0, actionBarEnd_ = 0, handStart_ = -1;
    std::string host_, username_, error_;
    std::string dimensionKey_;
    std::uint64_t dimensionSeed_ = 0;
    std::uint16_t port_ = 0;
    int compression_ = -1, minSection_ = -4, sectionCount_ = 24, sequence_ = 0;
    int chatOffset_ = 0;
    int entityId_ = 0;
    bool sentSprint_ = false;
    std::uint8_t sentInput_ = 0xff;
    std::uint32_t chatSeen_ = 0;
    double deadline_ = 0;
    double loadDeadline_ = 0;
    std::uint64_t positionRevision_ = 0;
    std::uint64_t worldRevision_ = 0;
    bool hasPosition_ = false, loaded_ = false;
    State state_ = State::Idle;
};
} // namespace craft
