#include "browser.hpp"
#include "movement.hpp"
#include "render.hpp"
#include "session.hpp"
#include "text_draw.hpp"
#include "avatars.hpp"
#include "screen_ui.hpp"
#include "game_ui.hpp"
#include "pause_menu.hpp"
#include <raylib.h>
#include <raymath.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#if defined(__PSP__)
#include <rlgl.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputility.h>
PSP_MODULE_INFO(CraftNet, PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
// Leave external memory for the Wi-Fi dialog, OSK and their system threads.
PSP_HEAP_THRESHOLD_SIZE_KB(8 * 1024);
#endif

namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr double BreakSeconds = 1.0;
constexpr float BaseFov = 70.0f;
constexpr float SprintFov = BaseFov * 1.15f;
enum class Screen
{
    Server,
    Connecting,
    RemoteGame
};
struct Target
{
    int x = 0, y = 0, z = 0, face = 0;
    float hx = 0.5f, hy = 0.5f, hz = 0.5f;
    float distance = 5;
    craft::BlockState state = 0;
    bool hit = false;
};

// Returns whether a gamepad button was pressed this frame.
bool pressed(int b)
{
    return IsGamepadAvailable(0) && IsGamepadButtonPressed(0, b);
}

#if !defined(__PSP__)
// Encodes one Unicode code point as UTF-8 and appends it.
void appendUtf8(std::string &s, unsigned c)
{
    if (c < 0x20 || c == 0x7f || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
    {
        return;
    }
    if (c < 0x80)
    {
        s.push_back(static_cast<char>(c));
    }
    else if (c < 0x800)
    {
        s.push_back(static_cast<char>(0xc0 | (c >> 6)));
        s.push_back(static_cast<char>(0x80 | (c & 63)));
    }
    else if (c < 0x10000)
    {
        s.push_back(static_cast<char>(0xe0 | (c >> 12)));
        s.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        s.push_back(static_cast<char>(0x80 | (c & 63)));
    }
    else
    {
        s.push_back(static_cast<char>(0xf0 | (c >> 18)));
        s.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
        s.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        s.push_back(static_cast<char>(0x80 | (c & 63)));
    }
}

#endif

// Returns the world-space view direction for a player pose.
Vector3 direction(const craft::PlayerPose &p)
{
    const float y = p.yaw * Pi / 180, pitch = p.pitch * Pi / 180;
    return {-std::sin(y) * std::cos(pitch), -std::sin(pitch), std::cos(y) * std::cos(pitch)};
}

// Traverse crossed cells and select the actual outline shape.
Target pick(const craft::World &world, const craft::WorldRenderer &render, const craft::PlayerPose &p, float eyeHeight)
{
    const auto d = direction(p);
    const double origin[3] = {p.x, p.y + eyeHeight, p.z};
    const float ray[3] = {d.x, d.y, d.z};
    int cell[3], step[3];
    float next[3], delta[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        cell[axis] = static_cast<int>(std::floor(origin[axis]));
        step[axis] = ray[axis] >= 0 ? 1 : -1;
        delta[axis] = std::abs(ray[axis]) > 0.000001f ? std::abs(1 / ray[axis]) : 1e20f;
        next[axis] = static_cast<float>((step[axis] > 0 ? cell[axis] + 1 - origin[axis] : origin[axis] - cell[axis]) *
                                        delta[axis]);
    }
    float entered = 0;
    for (int iteration = 0; iteration < 32 && entered <= 5; ++iteration)
    {
        const float exit = std::min({next[0], next[1], next[2], 5.0f});
        Target result;
        for (int below = 0; below <= 1; ++below)
        {
            const int y = cell[1] - below;
            craft::BlockState state;
            if (!world.get(cell[0], y, cell[2], state))
            {
                continue;
            }
            const Vector3 local{static_cast<float>(origin[0] - cell[0]), static_cast<float>(origin[1] - y),
                                static_cast<float>(origin[2] - cell[2])};
            float distance = 5;
            int face = 1;
            if (render.intersectRay(state, local, d, 5, distance, face) && distance >= entered - 0.001f &&
                distance <= exit + 0.001f && (!result.hit || distance < result.distance))
            {
                result = {cell[0],
                          y,
                          cell[2],
                          face,
                          local.x + d.x * distance,
                          local.y + d.y * distance,
                          local.z + d.z * distance,
                          distance,
                          state,
                          true};
            }
        }
        if (result.hit)
        {
            return result;
        }
        const int axis = next[0] <= next[1] && next[0] <= next[2] ? 0 : next[1] <= next[2] ? 1 : 2;
        entered = next[axis];
        next[axis] += delta[axis];
        cell[axis] += step[axis];
    }
    return {};
}

// Keep mining the same block when the crosshair moves across its faces.
bool same(const Target &a, const Target &b)
{
    return a.hit && b.hit && a.x == b.x && a.y == b.y && a.z == b.z && a.state == b.state;
}

} // namespace

// Runs the game loop for the PSP and desktop builds.
int main()
{
    using craft::Action;
#if defined(__PSP__)
    scePowerSetClockFrequency(333, 333, 166);
#endif
#if defined(_WIN32)
    ChangeDirectory(GetApplicationDirectory());
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(960, 544, "CraftNet");
    SetWindowMinSize(480, 272);
    MaximizeWindow();
    bool restoreMaximized = false;
#else
    InitWindow(480, 272, "CraftNet");
#endif
    SetExitKey(KEY_NULL);
    {
        craft::GameSettings settings;
        settings.load();
        SetTargetFPS(settings.fps);
        craft::PauseMenu pauseMenu;
        craft::ServerBrowser browser;
        auto session = std::make_unique<craft::Session>();
        session->setViewDistance((settings.viewBlocks + 15) / 16);
#if defined(__PSP__)
        rlSetClipPlanes(0.1, settings.viewBlocks);
#endif
        auto renderer = std::make_unique<craft::WorldRenderer>();
        const bool resources = renderer->load("assets/imported/compiled");
        auto avatars = std::make_unique<craft::Avatars>();
        avatars->load();
        craft::GameUi gameUi;
        gameUi.load();
        const Font chatFont = avatars->uiFont();
        craft::Movement movement;
        Screen screen = Screen::Server;
        std::string notice;
        Target breaking{};
        double breakStarted = 0;
        double lastAttack = -1;
        bool sprint = false;
        float cameraFov = BaseFov;
        bool hotbarTriggerHeld = false;
        std::uint64_t remoteRevision = 0;
        std::uint64_t worldRevision = 0;
        bool chatOpen = false;
        bool inventoryWasOpen = false;
        bool deathWasOpen = false;
        std::string chatDraft;
        std::array<std::string, 8> chatLayoutText;
        std::array<std::vector<craft::WrappedTextLine>, 8> chatLayout;
        std::string draftLayoutText;
        std::vector<craft::WrappedTextLine> draftLayout;
        craft::Keyboard keyboard;
        browser.start();
        while (!WindowShouldClose())
        {
#if defined(_WIN32)
            if (IsKeyPressed(KEY_F11))
            {
                if (!IsWindowFullscreen())
                {
                    restoreMaximized = IsWindowMaximized();
                    if (restoreMaximized)
                    {
                        // Save the normal window bounds before entering fullscreen.
                        RestoreWindow();
                    }
                }
                ToggleFullscreen();
                if (!IsWindowFullscreen() && restoreMaximized)
                {
                    MaximizeWindow();
                }
            }
#endif
            session->update();
            const bool keyboardHandled = keyboard.active || keyboard.completed;
            if (chatOpen && keyboard.completed)
            {
                if (keyboard.accepted && !keyboard.result.empty())
                {
                    session->sendChat(keyboard.result);
                }
                keyboard.completed = false;
                chatOpen = false;
            }
            const float dt = std::min(GetFrameTime(), 0.1f);
            const float simulationDt = std::clamp(GetFrameTime(), 0.0f, 0.25f);
            if (worldRevision != session->worldRevision())
            {
                worldRevision = session->worldRevision();
                avatars->reset();
                renderer->reset();
                movement.reset();
                breaking = {};
            }
            Target target{};
            if (screen == Screen::Connecting)
            {
                if (session->state() == craft::Session::State::Failed)
                {
                    notice = session->error();
                    screen = Screen::Server;
                }
                else if (session->state() == craft::Session::State::Play && session->hasPosition())
                {
                    movement.reset();
                    breaking = {};
                    sprint = false;
                    remoteRevision = session->positionRevision();
                    screen = Screen::RemoteGame;
                    DisableCursor();
                }
            }
            if (screen == Screen::RemoteGame && session->state() != craft::Session::State::Play)
            {
                const bool disconnected = session->state() == craft::Session::State::Failed ||
                                          session->state() == craft::Session::State::Idle;
                notice = disconnected ? session->error() : "";
                screen = disconnected ? Screen::Server : Screen::Connecting;
                renderer->reset();
                movement.reset();
                breaking = {};
                EnableCursor();
                chatOpen = false;
                pauseMenu.close();
            }
            if (screen == Screen::RemoteGame && !session->hasPosition())
            {
                screen = Screen::Connecting;
                renderer->reset();
                movement.reset();
                breaking = {};
                chatOpen = false;
                pauseMenu.close();
                EnableCursor();
            }
            bool menuHandled = pauseMenu.active();
            if (screen == Screen::RemoteGame && !keyboardHandled && !chatOpen)
            {
                if (pauseMenu.active())
                {
                    const int previousView = settings.viewBlocks;
                    const auto result = pauseMenu.update(*session, settings);
                    if (previousView != settings.viewBlocks)
                    {
                        session->setViewDistance((settings.viewBlocks + 15) / 16);
                        renderer->reset();
#if defined(__PSP__)
                        rlSetClipPlanes(0.1, settings.viewBlocks);
#endif
                    }
                    if (result == craft::PauseMenu::Result::Resume && !session->dead() && !session->inventory().open)
                    {
                        DisableCursor();
                        GetMouseDelta();
                    }
                    else if (result == craft::PauseMenu::Result::Disconnect)
                    {
                        session->disconnect();
                        renderer->reset();
                        movement.reset();
                        screen = Screen::Server;
                    }
                }
                else if (IsKeyPressed(KEY_ESCAPE) || pressed(GAMEPAD_BUTTON_MIDDLE_RIGHT))
                {
                    if (breaking.hit)
                    {
                        session->blockAction(2, breaking.x, breaking.y, breaking.z, breaking.face);
                        breaking = {};
                    }
                    if (session->inventory().open)
                    {
                        session->closeInventory();
                    }
                    pauseMenu.open();
                    EnableCursor();
                    menuHandled = true;
                }
            }
            const bool remoteGame = screen == Screen::RemoteGame;
            const bool deathScreen = remoteGame && session->dead();
            const bool hotbarModifier = settings.padHeld(Action::HotbarModifier);
            const bool openInventory = remoteGame && !deathScreen && !session->spectator() && !chatOpen &&
                                       !menuHandled && !keyboardHandled && !session->inventory().open &&
                                       (settings.desktopPressed(Action::Inventory) ||
                                        (!hotbarModifier && settings.padPressed(Action::Inventory)));
            if (openInventory)
            {
                session->openInventory();
            }
            const bool inventoryHandled = remoteGame && session->inventory().open;
            if (inventoryHandled)
            {
                chatOpen = false;
                if (breaking.hit)
                {
                    session->blockAction(2, breaking.x, breaking.y, breaking.z, breaking.face);
                    breaking = {};
                }
                if (!keyboardHandled && !menuHandled)
                {
                    gameUi.update(*session, !inventoryWasOpen, settings.desktopPressed(Action::Inventory));
                }
            }
            const bool inventoryOpen = remoteGame && session->inventory().open;
            if (remoteGame && inventoryOpen != inventoryWasOpen)
            {
                if (inventoryOpen)
                {
                    EnableCursor();
                }
                else if (!chatOpen && !deathScreen && !pauseMenu.active())
                {
                    DisableCursor();
                }
            }
            inventoryWasOpen = inventoryOpen;
            if (remoteGame && deathScreen != deathWasOpen)
            {
                if (deathScreen)
                {
                    chatOpen = false;
                    EnableCursor();
                }
                else if (!inventoryOpen && !pauseMenu.active())
                {
                    DisableCursor();
                }
            }
            deathWasOpen = deathScreen;
            if (deathScreen && !keyboardHandled && !menuHandled)
            {
                const bool clickRespawn = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                    CheckCollisionPointRec(GetScreenToWorld2D(GetMousePosition(), craft::uiCamera()), {151, 152, 178, 24});
                if (clickRespawn || IsKeyPressed(KEY_ENTER) || pressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
                {
                    session->requestRespawn();
                }
            }
            bool chatHandled = chatOpen || keyboardHandled || inventoryHandled || deathScreen || menuHandled;
            if (screen == Screen::RemoteGame && !chatHandled &&
                settings.pressed(Action::Chat))
            {
                chatHandled = true;
                if (breaking.hit)
                {
                    session->blockAction(2, breaking.x, breaking.y, breaking.z, breaking.face);
                    breaking = {};
                }
#if defined(__PSP__)
                chatOpen = keyboard.open("Server chat / command", "", 256);
#else
                chatDraft.clear();
                chatOpen = true;
                EnableCursor();
                while (GetCharPressed() > 0)
                {
                }
#endif
            }
#if !defined(__PSP__)
            if (screen == Screen::RemoteGame && chatOpen)
            {
                int ch;
                while ((ch = GetCharPressed()) > 0)
                {
                    std::size_t count = 0;
                    for (unsigned char c : chatDraft)
                    {
                        if ((c & 0xc0) != 0x80)
                        {
                            ++count;
                        }
                    }
                    if (count < 256)
                    {
                        appendUtf8(chatDraft, static_cast<unsigned>(ch));
                    }
                }
                if (IsKeyPressed(KEY_BACKSPACE) && !chatDraft.empty())
                {
                    do
                    {
                        unsigned char c = static_cast<unsigned char>(chatDraft.back());
                        chatDraft.pop_back();
                        if ((c & 0xc0) != 0x80)
                        {
                            break;
                        }
                    } while (!chatDraft.empty());
                }
                if (IsKeyPressed(KEY_ENTER))
                {
                    session->sendChat(chatDraft);
                    chatOpen = false;
                    DisableCursor();
                }
                else if (IsKeyPressed(KEY_ESCAPE))
                {
                    chatOpen = false;
                    DisableCursor();
                }
            }
#endif
            if (remoteGame && chatHandled && !deathScreen)
            {
                auto pose = session->player();
                if (remoteRevision != session->positionRevision())
                {
                    movement.reset(true);
                    remoteRevision = session->positionRevision();
                }
                movement.update(pose, session->world(), *renderer, {}, simulationDt, session.get());
                session->setPose(pose);
            }
            if (remoteGame && !chatOpen && !chatHandled)
            {
                auto pose = session->player();
                const auto &world = session->world();
                if (remoteRevision != session->positionRevision())
                {
                    movement.reset(true);
                    remoteRevision = session->positionRevision();
                }
                auto mouse = GetMouseDelta();
                const float sensitivity = settings.sensitivity / 100.0f;
                pose.yaw += mouse.x * 0.15f * sensitivity;
                pose.pitch += mouse.y * 0.15f * sensitivity;
                if (settings.padHeld(Action::LookLeft))
                {
                    pose.yaw -= 115.0f * dt * sensitivity;
                }
                if (settings.padHeld(Action::LookRight))
                {
                    pose.yaw += 115.0f * dt * sensitivity;
                }
                if (settings.padHeld(Action::LookUp))
                {
                    pose.pitch -= 90.0f * dt * sensitivity;
                }
                if (settings.padHeld(Action::LookDown))
                {
                    pose.pitch += 90.0f * dt * sensitivity;
                }
                pose.yaw = std::remainder(pose.yaw, 360.0f);
                pose.pitch = std::max(-89.0f, std::min(89.0f, pose.pitch));
                craft::MovementInput input;
                input.forward = settings.held(Action::Forward) - settings.held(Action::Backward);
                input.side = settings.held(Action::StrafeRight) - settings.held(Action::StrafeLeft);
                if (IsGamepadAvailable(0))
                {
                    input.side += GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
                    input.forward -= GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
                }
                if (hotbarModifier && settings.padPressed(Action::Sprint))
                {
                    sprint = !sprint;
                }
                input.jump = settings.desktopPressed(Action::Jump) || (!hotbarModifier && settings.padPressed(Action::Jump));
                input.jumpHeld = settings.desktopHeld(Action::Jump) || (!hotbarModifier && settings.padHeld(Action::Jump));
                input.sprint = settings.desktopHeld(Action::Sprint) || sprint;
                input.sneak = settings.held(Action::Sneak);
                movement.update(pose, world, *renderer, input, simulationDt, session.get());
                session->setPose(pose);
                const bool canAct = !session->spectator();
                if (canAct)
                {
                    target = pick(world, *renderer, pose, movement.eyeHeight());
                }
                int slot = session->inventory().selected;
                for (int i = 0; i < 9; ++i)
                {
                    if (settings.pressed(static_cast<Action>(static_cast<int>(Action::Slot1) + i)))
                    {
                        slot = i;
                    }
                }
                const float wheel = GetMouseWheelMove();
                if (wheel || hotbarModifier)
                {
                    const int step = wheel > 0 || (hotbarModifier && settings.padPressed(Action::PreviousSlot)) ? -1
                                   : wheel < 0 || (hotbarModifier && settings.padPressed(Action::NextSlot)) ? 1 : 0;
                    slot = (slot + step + 9) % 9;
                }
                if (canAct)
                {
                    session->selectHotbar(slot);
                }
                hotbarTriggerHeld = settings.padHeld(Action::Attack) && (hotbarModifier || hotbarTriggerHeld);
                const bool held = canAct && (settings.desktopHeld(Action::Attack) ||
                                             (!hotbarTriggerHeld && settings.padHeld(Action::Attack)));
                const bool use = canAct && (settings.desktopPressed(Action::Use) ||
                                            (!hotbarModifier && settings.padPressed(Action::Use)));
                int entity = -1;
                float nearest = target.hit ? std::min(3.0f, target.distance) : 3.0f;
                if (held || use)
                {
                    const Ray ray{
                        {static_cast<float>(pose.x), static_cast<float>(pose.y + movement.eyeHeight()),
                         static_cast<float>(pose.z)},
                        direction(pose)};
                    for (const auto &remote : session->players())
                    {
                        if (!remote.active || remote.spectator || remote.marker ||
                            remote.type == craft::remoteType::TextDisplay ||
                            (remote.invisible && remote.type != craft::remoteType::Interaction))
                        {
                            continue;
                        }
                        const float halfWidth = remote.width * (remote.small ? 0.25f : 0.5f);
                        const float height = remote.height * (remote.small ? 0.5f : 1.0f);
                        const BoundingBox box{{static_cast<float>(remote.x - halfWidth), static_cast<float>(remote.y),
                                               static_cast<float>(remote.z - halfWidth)},
                                              {static_cast<float>(remote.x + halfWidth),
                                               static_cast<float>(remote.y + height),
                                               static_cast<float>(remote.z + halfWidth)}};
                        const auto hit = GetRayCollisionBox(ray, box);
                        if (hit.hit && hit.distance < nearest)
                        {
                            entity = remote.id;
                            nearest = hit.distance;
                        }
                    }
                    if (held && entity >= 0 && GetTime() - lastAttack >= 0.4)
                    {
                        session->attackEntity(entity);
                        lastAttack = GetTime();
                    }
                }
                if (held && entity < 0 && target.hit)
                {
                    if (!same(target, breaking))
                    {
                        if (breaking.hit)
                        {
                            session->blockAction(2, breaking.x, breaking.y, breaking.z, breaking.face);
                        }
                        breaking = target;
                        breakStarted = GetTime();
                        session->blockAction(0, target.x, target.y, target.z, target.face);
                    }
                    else if (GetTime() - breakStarted >= BreakSeconds)
                    {
                        session->blockAction(3, breaking.x, breaking.y, breaking.z, breaking.face);
                        breaking = {};
                    }
                    else if (session->handSwing() >= 1)
                    {
                        session->swingHand();
                    }
                }
                else if (breaking.hit)
                {
                    session->blockAction(2, breaking.x, breaking.y, breaking.z, breaking.face);
                    breaking = {};
                }
                if (use && entity >= 0)
                {
                    session->interactEntity(entity, input.sneak);
                }
                else if (target.hit && use)
                {
                    session->placeBlock(target.x, target.y, target.z, target.face, std::clamp(target.hx, 0.0f, 1.0f),
                                        std::clamp(target.hy, 0.0f, 1.0f), std::clamp(target.hz, 0.0f, 1.0f));
                }
                else if (use)
                {
                    session->useItem();
                }
                if (held && entity < 0 && !target.hit &&
                    settings.pressed(Action::Attack))
                {
                    session->swingHand(true);
                }
                if (canAct && (settings.desktopPressed(Action::Drop) || (hotbarModifier && settings.padPressed(Action::Drop))))
                {
                    session->dropItem(settings.desktopHeld(Action::Sprint));
                }
            }
            else if (screen == Screen::Server && !menuHandled)
            {
                if (browser.update(keyboard, notice) && resources)
                {
                    const auto &entry = browser.selected();
                    notice.clear();
                    if (session->connect(entry.host, entry.port, browser.username()))
                    {
                        screen = Screen::Connecting;
                    }
                    else
                    {
                        notice = session->error();
                    }
                }
            }
            if (screen == Screen::Connecting && !keyboardHandled && !craft::Transport::wifiDialogActive() &&
                (IsKeyPressed(KEY_ESCAPE) || pressed(GAMEPAD_BUTTON_MIDDLE_RIGHT)))
            {
                session->disconnect();
                renderer->reset();
                screen = Screen::Server;
            }
            BeginDrawing();
            ClearBackground(Color{20, 29, 33, 255});
            const bool drawRemote = screen == Screen::RemoteGame && !keyboard.active;
            if (!drawRemote)
            {
                cameraFov = BaseFov;
            }
            if (keyboard.active)
            {
                craft::drawClippedText(chatFont, "System keyboard", {18, 30}, 20, 444, RAYWHITE);
            }
            else if (drawRemote)
            {
                auto p = movement.visual(session->player());
                float eyeHeight = movement.eyeHeight();
                if (const auto *tracked = session->cameraTarget())
                {
                    p.x = tracked->x;
                    p.y = tracked->y;
                    p.z = tracked->z;
                    p.yaw = tracked->headYaw;
                    p.pitch = tracked->pitch;
                    eyeHeight = tracked->sneaking ? 1.27f : 1.62f;
                }
                const auto &world = session->world();
                Camera3D camera{};
                camera.position = {static_cast<float>(p.x), static_cast<float>(p.y + eyeHeight),
                                   static_cast<float>(p.z)};
                camera.up = {0, 1, 0};
                if (settings.bob && !session->spectator() && !session->dead())
                {
                    const auto bob = movement.viewBob();
                    const auto right = Vector3Normalize(Vector3CrossProduct(direction(p), camera.up));
                    camera.position = Vector3Add(camera.position, Vector3Scale(right, bob.side));
                    camera.position.y += bob.up;
                    p.pitch += bob.pitch;
                    camera.up = Vector3RotateByAxisAngle(camera.up, direction(p), bob.roll * DEG2RAD);
                }
                const auto d = direction(p);
                camera.target = Vector3Add(camera.position, d);
                const bool sprinting = (p.input & 64) && !session->spectator() && !session->dead();
                const float targetFov = sprinting ? SprintFov : BaseFov;
                cameraFov += (targetFov - cameraFov) * (1.0f - std::exp2(-dt / 0.07f));
                camera.fovy = cameraFov;
                camera.projection = CAMERA_PERSPECTIVE;
                DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(), Color{72, 131, 193, 255},
                                       Color{170, 204, 213, 255});
                BeginMode3D(camera);
                renderer->draw(world, camera.position, d, camera.fovy, settings.viewBlocks);
                if (breaking.hit)
                {
                    const int stage = std::clamp(static_cast<int>((GetTime() - breakStarted) / BreakSeconds * 10), 0, 9);
                    renderer->drawCracks(breaking.state, breaking.x, breaking.y, breaking.z, gameUi.crackAtlas(),
                                         {16.0f * stage, 448, 16, 16});
                }
                avatars->draw(*session, camera, d, dt, gameUi);
                gameUi.drawDrops(*session, camera);
                if (target.hit)
                {
                    renderer->drawOutline(target.state, target.x, target.y, target.z);
                }
                EndMode3D();
                const auto hudCamera = craft::uiCamera();
                const float hudLeft = -hudCamera.offset.x / hudCamera.zoom;
                const float hudTop = -hudCamera.offset.y / hudCamera.zoom;
                const float hudRight = (GetScreenWidth() - hudCamera.offset.x) / hudCamera.zoom;
#if defined(_WIN32)
                BeginMode2D(hudCamera);
#endif
                if (!pauseMenu.active() && !inventoryOpen && !session->dead())
                {
                    if (!session->spectator())
                    {
                        const auto &inv = session->inventory();
                        gameUi.drawHand(inv.hotbar(inv.selected), avatars->handSkin(), session->handSwing());
                    }
                    DrawLine(236, 136, 244, 136, BLACK);
                    DrawLine(240, 132, 240, 140, BLACK);
                }
                if (!session->spectator() && !session->dead())
                {
                    gameUi.drawHud(session->inventory(), chatFont);
                }
                if (settings.scoreboard && !pauseMenu.active() && !inventoryOpen && !session->dead())
                {
                    gameUi.drawSidebar(session->sidebar(), chatFont, hudRight, hudTop);
                }
                constexpr int ChatWidth = 272;
                constexpr int ChatLineHeight = 13;
                constexpr float ChatFontSize = 11;
                const auto &history = session->chat();
                if (chatOpen && draftLayoutText != chatDraft)
                {
                    draftLayoutText = chatDraft;
                    craft::wrapGameText(chatFont, chatDraft, ChatFontSize, ChatWidth - 8, RAYWHITE, draftLayout);
                }
                const int draftRows = std::clamp(static_cast<int>(draftLayout.size()), 1, 3);
                const int draftTop = 226 - (draftRows * ChatLineHeight + 6);
                const int bottom = chatOpen ? draftTop - 1 : 227;
                const int rowLimit = (settings.chat || chatOpen) && !pauseMenu.active() ? (chatOpen ? 12 : 8) : 0;
                const int messageLimit = chatOpen ? static_cast<int>(history.size()) : 4;
                int row = 0, messages = 0;
                for (int i = static_cast<int>(history.size()) - 1; i >= 0 && row < rowLimit && messages < messageLimit; --i)
                {
                    if (chatLayoutText[i] != history[i])
                    {
                        chatLayoutText[i] = history[i];
                        craft::wrapGameText(chatFont, history[i], ChatFontSize, ChatWidth - 6, RAYWHITE, chatLayout[i]);
                    }
                    const auto &lines = chatLayout[i];
                    messages += !lines.empty();
                    for (auto it = lines.rbegin(); it != lines.rend() && row < rowLimit; ++it)
                    {
                        const float y = static_cast<float>(bottom - (++row) * ChatLineHeight);
                        DrawRectangleRec({hudLeft, y - 1, ChatWidth, ChatLineHeight}, Color{0, 0, 0, 150});
                        craft::beginUiScissor(hudLeft + 3, y, ChatWidth - 6, ChatLineHeight);
                        craft::drawClippedText(chatFont, it->text.c_str(), {hudLeft + 3, y}, ChatFontSize,
                                               ChatWidth - 6, RAYWHITE, &*it);
                        EndScissorMode();
                    }
                }
                if (chatOpen)
                {
                    DrawRectangleRec({hudLeft, static_cast<float>(draftTop), ChatWidth,
                                      static_cast<float>(226 - draftTop)}, Color{0, 0, 0, 200});
                    craft::beginUiScissor(hudLeft + 4, draftTop + 3, ChatWidth - 8, draftRows * ChatLineHeight);
                    float y = static_cast<float>(draftTop + 3);
                    const int first = std::max(0, static_cast<int>(draftLayout.size()) - draftRows);
                    for (int i = first; i < static_cast<int>(draftLayout.size()); ++i, y += ChatLineHeight)
                    {
                        const auto &line = draftLayout[i];
                        craft::drawClippedText(chatFont, line.text.c_str(), {hudLeft + 4, y}, ChatFontSize,
                                               ChatWidth - 8, RAYWHITE, &line);
                    }
                    EndScissorMode();
                }
                if (inventoryOpen && !pauseMenu.active())
                {
                    gameUi.drawInventory(session->inventory(), chatFont);
                }
                gameUi.drawEffects(*session, chatFont);
                pauseMenu.draw(chatFont, settings);
#if defined(_WIN32)
                EndMode2D();
#endif
            }
            else
            {
#if defined(_WIN32)
                BeginMode2D(craft::uiCamera());
#endif
                browser.draw(chatFont, screen == Screen::Connecting, notice, resources);
#if defined(_WIN32)
                EndMode2D();
#endif
            }
            avatars->pump(screen == Screen::RemoteGame && !keyboard.active);
            session->flush();
            browser.pump(screen == Screen::Server);
            keyboard.update();
            EndDrawing();
        }
        browser.cancelRefresh();
        session->disconnect();
        renderer->reset();
    }
    CloseWindow();
    return 0;
}
