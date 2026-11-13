#pragma once
#include "session.hpp"
#include "skin_cache.hpp"
#include <raymath.h>
#include <algorithm>
#include <cmath>

namespace craft
{
class GameUi;
class Avatars final
{
  public:
    ~Avatars();
    void load();
    void draw(const Session &session, Camera3D camera, Vector3 forward, float dt, const GameUi &ui);
    Font uiFont();
    Texture2D handSkin() { return skins_.texture("", false, false); }
    void pump(bool playing);
    void reset();

  private:
    struct Pose
    {
        int id = -1;
        Vector3 position{};
        float body = 0, head = 0, pitch = 0, crouch = 0;
        float phase = 0, speed = 0, swing = 1, rightAttack = 0, leftAttack = 0;
        unsigned event = 0;
        float stride() const { return std::sin(phase) * std::min(speed / 5.0f, 1.0f) * 28; }
    };
    struct Glyph
    {
        std::uint16_t x = 0, y = 0;
        std::uint8_t width = 0, advance = 0;
    };
    void model(const RemotePlayer &player, const Pose &pose, Texture2D texture, bool detail, bool slim,
               const GameUi *ui = nullptr);
    void npcModel(const RemotePlayer &player, const Pose &pose);
    int name(const RemotePlayer &player, Vector3 position, Vector3 right, Vector3 up, int budget, bool draw = true);
    SkinCache skins_;
    std::array<Pose, RemoteEntityLimit> poses_{};
    std::array<Texture2D, 4> npcTextures_{};
    Texture2D armor_{};
    std::array<Glyph, 1024> glyphs_{};
    std::array<std::uint16_t, 0x2800> glyphIndex_{};
    Texture2D font_{};
    std::array<Rectangle, 1024> uiRects_{};
    std::array<GlyphInfo, 1024> uiGlyphs_{};
    int uiGlyphCount_ = 0;
};
} // namespace craft
