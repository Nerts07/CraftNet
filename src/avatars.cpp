#include "avatars.hpp"
#include "game_ui.hpp"
#include <rlgl.h>
#if defined(__PSP__)
#include <GL/gl.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace craft
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
float angle(float from, float to, float factor)
{
    return from + std::remainder(to - from, 360.0f) * std::min(1.0f, factor);
}
// Emit a quad as triangles: PSP has no native GL_QUADS primitive.
void quad(const Vector3 *v, float u, float t, float w, float h, Color color, bool mirror = false)
{
    const float uv[4][2] = {{u, t + h}, {u + w, t + h}, {u + w, t}, {u, t}};
    constexpr unsigned indices[6] = {0, 1, 2, 0, 2, 3};
    rlColor4ub(color.r, color.g, color.b, color.a);
    for (unsigned i : indices)
    {
        rlTexCoord2f(uv[mirror ? (i ^ 1) : i][0], uv[i][1]);
        rlVertex3f(v[i].x, v[i].y, v[i].z);
    }
}
struct BoxStyle
{
    Color color = WHITE;
    Vector2 offset{};
    Vector2 step{1.0f / 64, 1.0f / 64};
};

void box(Matrix transform, Vector3 low, Vector3 high, int u, int v, int w, int h, int d, float inflate,
         const BoxStyle &style = {})
{
    low = Vector3SubtractValue(low, inflate);
    high = Vector3AddValue(high, inflate);
    Vector3 vertices[8] = {{low.x, low.y, low.z},    {high.x, low.y, low.z}, {high.x, high.y, low.z},
                           {low.x, high.y, low.z},   {low.x, low.y, high.z}, {high.x, low.y, high.z},
                           {high.x, high.y, high.z}, {low.x, high.y, high.z}};
    for (auto &vertex : vertices)
    {
        vertex = Vector3Transform(vertex, transform);
    }
    // Seen from -Z, the front's left edge is at +X. The skin strip runs
    // from that side through the front, the -X side and then the back.
    constexpr unsigned faces[6][4] = {{1, 0, 3, 2}, {4, 5, 6, 7}, {0, 4, 7, 3},
                                      {5, 1, 2, 6}, {3, 7, 6, 2}, {4, 0, 1, 5}};
    const int uv[6][4] = {{u + d, v + d, w, h}, {u + d + w + d, v + d, w, h},
                          {u + d + w, v + d, d, h}, {u, v + d, d, h},
                          {u + d, v, w, d},     {u + d + w, v, w, d}};
    constexpr unsigned shades[6] = {225, 225, 195, 195, 255, 165};
    for (unsigned face = 0; face < 6; ++face)
    {
        Vector3 points[4];
        for (unsigned k = 0; k < 4; ++k)
        {
            points[k] = vertices[faces[face][k]];
        }
        const auto shade = static_cast<unsigned char>(shades[face]);
        const Color color{static_cast<unsigned char>(style.color.r * shade / 255),
                          static_cast<unsigned char>(style.color.g * shade / 255),
                          static_cast<unsigned char>(style.color.b * shade / 255), style.color.a};
        quad(points, style.offset.x + uv[face][0] * style.step.x, style.offset.y + uv[face][1] * style.step.y,
             uv[face][2] * style.step.x, uv[face][3] * style.step.y, color);
    }
}
Matrix part(Matrix root, Vector3 pivot, float x, float y = 0)
{
    Matrix matrix = MatrixMultiply(MatrixRotateX(x * DEG2RAD), MatrixRotateY(y * DEG2RAD));
    matrix = MatrixMultiply(matrix, MatrixTranslate(pivot.x, pivot.y, pivot.z));
    return MatrixMultiply(matrix, root);
}
constexpr Color Colors[16] = {{0, 0, 0, 255},     {0, 0, 170, 255},    {0, 170, 0, 255},    {0, 170, 170, 255},
                              {170, 0, 0, 255},   {170, 0, 170, 255},  {255, 170, 0, 255},  {170, 170, 170, 255},
                              {85, 85, 85, 255},  {85, 85, 255, 255},  {85, 255, 85, 255},  {85, 255, 255, 255},
                              {255, 85, 85, 255}, {255, 85, 255, 255}, {255, 255, 85, 255}, {255, 255, 255, 255}};
int hex(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

Vector3 labelPosition(const RemotePlayer &entity, Vector3 position)
{
    if (entity.type == remoteType::TextDisplay)
    {
        return Vector3Add(position, {entity.labelOffset[0], entity.labelOffset[1], entity.labelOffset[2]});
    }
    const float height = entity.marker ? 0.0f : (entity.sneaking ? 1.8f : 2.2f) * (entity.small ? 0.5f : 1.0f);
    return Vector3Add(position, {0, height, 0});
}

int armorMaterial(std::string_view name)
{
    constexpr std::string_view materials[] = {"leather ", "chainmail ", "iron ", "golden ", "diamond ",
                                              "netherite ", "copper ", "turtle "};
    for (int i = 0; i < 8; ++i)
    {
        if (name.substr(0, materials[i].size()) == materials[i])
        {
            return i;
        }
    }
    return -1;
}
} // namespace
Avatars::~Avatars()
{
    if (armor_.id)
    {
        UnloadTexture(armor_);
    }
    for (const auto texture : npcTextures_)
    {
        if (texture.id)
        {
            UnloadTexture(texture);
        }
    }
    if (font_.id)
    {
        UnloadTexture(font_);
    }
}
void Avatars::load()
{
    skins_.load();
    constexpr const char *armorPath = "assets/imported/compiled/inventory-armor.png";
    if (FileExists(armorPath))
    {
        Image image = LoadImage(armorPath);
        if (image.data && image.width == 256 && image.height == 128)
        {
#if defined(__PSP__)
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R4G4B4A4);
#endif
            armor_ = LoadTextureFromImage(image);
            SetTextureFilter(armor_, TEXTURE_FILTER_POINT);
        }
        UnloadImage(image);
    }
    constexpr const char *textures[] = {"villager", "wandering_trader", "zombie_villager", "armorstand"};
    for (std::size_t i = 0; i < npcTextures_.size(); ++i)
    {
        const auto path = std::string("assets/imported/compiled/") + textures[i] + ".png";
        if (!FileExists(path.c_str()))
        {
            continue;
        }
        Image image = LoadImage(path.c_str());
        if (image.data && image.width == 64 && image.height == 64)
        {
#if defined(__PSP__)
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R4G4B4A4);
#endif
            npcTextures_[i] = LoadTextureFromImage(image);
            SetTextureFilter(npcTextures_[i], TEXTURE_FILTER_POINT);
        }
        UnloadImage(image);
    }
    if (FILE *file = std::fopen("assets/imported/compiled/player-font.bin", "rb"))
    {
        std::uint32_t count;
        if (std::fread(&count, 4, 1, file) == 1 && count <= glyphs_.size())
        {
            unsigned fallback = 0;
            for (unsigned i = 0; i < count; ++i)
            {
                std::uint32_t code;
                auto &g = glyphs_[i];
                if (std::fread(&code, 4, 1, file) != 1 || std::fread(&g.x, 2, 1, file) != 1 ||
                    std::fread(&g.y, 2, 1, file) != 1 || std::fread(&g.width, 1, 1, file) != 1 ||
                    std::fread(&g.advance, 1, 1, file) != 1)
                {
                    break;
                }
                if (g.x > 248 || g.y > 248 || g.width > 8 || g.advance > 9)
                {
                    g = {};
                }
                if (code < glyphIndex_.size())
                {
                    glyphIndex_[code] = static_cast<std::uint16_t>(i + 1);
                }
                uiRects_[i] = {static_cast<float>(g.x), static_cast<float>(g.y), static_cast<float>(g.width), 8};
                uiGlyphs_[i].value = static_cast<int>(code);
                uiGlyphs_[i].advanceX = g.advance;
                uiGlyphCount_ = static_cast<int>(i + 1);
                if (code == '?')
                {
                    fallback = i + 1;
                }
            }
            for (auto &index : glyphIndex_)
            {
                if (!index)
                {
                    index = static_cast<std::uint16_t>(fallback);
                }
            }
        }
        std::fclose(file);
    }
    Image image = LoadImage("assets/imported/compiled/player-font.png");
    if (image.data && image.width == 256 && image.height == 256)
    {
#if defined(__PSP__)
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R4G4B4A4);
#endif
        font_ = LoadTextureFromImage(image);
        SetTextureFilter(font_, TEXTURE_FILTER_POINT);
    }
    UnloadImage(image);
}

Font Avatars::uiFont()
{
    return font_.id && uiGlyphCount_ ? Font{8, uiGlyphCount_, 0, font_, uiRects_.data(), uiGlyphs_.data()} :
                                      GetFontDefault();
}
void Avatars::reset()
{
    poses_ = {};
}
void Avatars::pump(bool playing)
{
    if (playing)
    {
        skins_.pump();
    }
    else
    {
        skins_.cancel();
    }
}
void Avatars::model(const RemotePlayer &player, const Pose &pose, Texture2D texture, bool detail, bool slim,
                    const GameUi *ui)
{
    const float scale = 0.9375f / 16;
    Matrix root = MatrixMultiply(MatrixScale(scale, scale, scale), MatrixRotateY((180 - pose.body) * DEG2RAD));
    root = MatrixMultiply(root, MatrixTranslate(pose.position.x, pose.position.y, pose.position.z));
    const float walk = pose.stride();
    const float lean = -25 * pose.crouch;
    const float drop = -3 * pose.crouch;
    const float armWidth = slim ? 3 : 4;
    const float armX = 4 + armWidth * 0.5f;
    const auto tint = static_cast<unsigned char>(255 - 165 * std::clamp((player.hurtUntil - GetTime()) / 0.45, 0.0, 1.0));
    const Color hurt{255, tint, tint, 255};
    const auto skinBox = [&](Matrix partMatrix, Vector3 low, Vector3 high, int u, int v, int w, int h, int d, float size)
    {
        box(partMatrix, low, high, u, v, w, h, d, size, {hurt});
    };
    rlSetTexture(texture.id);
    rlBegin(RL_TRIANGLES);
    const auto torso = part(root, {0, 12 + drop, 0}, lean);
    const auto head = part(root, {0, 24 + drop, -2 * pose.crouch},
                           -std::clamp(std::remainder(pose.pitch, 360.0f), -90.0f, 90.0f),
                           -std::clamp(std::remainder(pose.head - pose.body, 360.0f), -85.0f, 85.0f));
    const auto rightArm = part(root, {-armX, 22 + drop, 0}, walk - pose.rightAttack + lean);
    const auto leftArm = part(root, {armX, 22 + drop, 0}, -walk - pose.leftAttack + lean);
    const auto rightLeg = part(root, {-2, 12, 3 * pose.crouch}, -walk);
    const auto leftLeg = part(root, {2, 12, 3 * pose.crouch}, walk);
    skinBox(torso, {-4, 0, -2}, {4, 12, 2}, 16, 16, 8, 12, 4, 0);
    skinBox(head, {-4, 0, -4}, {4, 8, 4}, 0, 0, 8, 8, 8, 0);
    skinBox(rightArm, {-armWidth / 2, -10, -2}, {armWidth / 2, 2, 2}, 40, 16, static_cast<int>(armWidth), 12, 4, 0);
    skinBox(leftArm, {-armWidth / 2, -10, -2}, {armWidth / 2, 2, 2}, 32, 48, static_cast<int>(armWidth), 12, 4, 0);
    skinBox(rightLeg, {-2, -12, -2}, {2, 0, 2}, 0, 16, 4, 12, 4, 0);
    skinBox(leftLeg, {-2, -12, -2}, {2, 0, 2}, 16, 48, 4, 12, 4, 0);
    if (detail)
    {
        if (player.skinParts & 64)
        {
            skinBox(head, {-4, 0, -4}, {4, 8, 4}, 32, 0, 8, 8, 8, 0.5f);
        }
        if (player.skinParts & 2)
        {
            skinBox(torso, {-4, 0, -2}, {4, 12, 2}, 16, 32, 8, 12, 4, 0.25f);
        }
        if (player.skinParts & 8)
        {
            skinBox(rightArm, {-armWidth / 2, -10, -2}, {armWidth / 2, 2, 2}, 40, 32, static_cast<int>(armWidth), 12, 4,
                0.25f);
        }
        if (player.skinParts & 4)
        {
            skinBox(leftArm, {-armWidth / 2, -10, -2}, {armWidth / 2, 2, 2}, 48, 48, static_cast<int>(armWidth), 12, 4,
                0.25f);
        }
        if (player.skinParts & 32)
        {
            skinBox(rightLeg, {-2, -12, -2}, {2, 0, 2}, 0, 32, 4, 12, 4, 0.25f);
        }
        if (player.skinParts & 16)
        {
            skinBox(leftLeg, {-2, -12, -2}, {2, 0, 2}, 0, 48, 4, 12, 4, 0.25f);
        }
    }
    rlEnd();
    rlSetTexture(0);
    if (!ui || !armor_.id)
    {
        return;
    }
    for (int slot = 1; slot <= 4; ++slot)
    {
        const int material = armorMaterial(ui->itemName(player.equipment[slot]));
        if (material < 0)
        {
            continue;
        }
        Color armorTint = hurt;
        if (material == 0)
        {
            const unsigned rgb = player.armorDyes[slot] >= 0 ? player.armorDyes[slot] : 0xa06540;
            armorTint = {static_cast<unsigned char>(rgb >> 16), static_cast<unsigned char>(((rgb >> 8) & 255) * tint / 255),
                         static_cast<unsigned char>((rgb & 255) * tint / 255), 255};
        }
        const BoxStyle armor{armorTint, {static_cast<float>(material % 4) / 4,
                                     static_cast<float>(material / 4 + (slot == 2 ? 2 : 0)) / 4},
                             {1.0f / 256, 1.0f / 128}};
        rlSetTexture(armor_.id);
        rlBegin(RL_TRIANGLES);
        if (slot == 4)
        {
            box(head, {-4, 0, -4}, {4, 8, 4}, 0, 0, 8, 8, 8, 0.85f, armor);
        }
        else if (slot == 3)
        {
            box(torso, {-4, 0, -2}, {4, 12, 2}, 16, 16, 8, 12, 4, 0.85f, armor);
            box(rightArm, {-2, -10, -2}, {2, 2, 2}, 40, 16, 4, 12, 4, 0.85f, armor);
            box(leftArm, {-2, -10, -2}, {2, 2, 2}, 40, 16, 4, 12, 4, 0.85f, armor);
        }
        else
        {
            const float inflate = slot == 2 ? 0.48f : 0.85f;
            box(rightLeg, {-2, -12, -2}, {2, 0, 2}, 0, 16, 4, 12, 4, inflate, armor);
            box(leftLeg, {-2, -12, -2}, {2, 0, 2}, 0, 16, 4, 12, 4, inflate, armor);
            if (slot == 2)
            {
                box(torso, {-4, 0, -2}, {4, 12, 2}, 16, 16, 8, 12, 4, inflate, armor);
            }
        }
        rlEnd();
        rlSetTexture(0);
    }
}
void Avatars::npcModel(const RemotePlayer &player, const Pose &pose)
{
    const bool stand = player.type == remoteType::ArmorStand;
    const bool zombie = player.type == remoteType::ZombieVillager;
    const auto texture = npcTextures_[stand ? 3 : zombie ? 2 : player.type == remoteType::WanderingTrader ? 1 : 0];
    if (!texture.id)
    {
        model(player, pose, skins_.texture("", false, false), false, false);
        return;
    }
    const float scale = (player.small ? 0.5f : 1.0f) / 16;
    Matrix root = MatrixMultiply(MatrixScale(scale, scale, scale), MatrixRotateY((180 - pose.body) * DEG2RAD));
    root = MatrixMultiply(root, MatrixTranslate(pose.position.x, pose.position.y, pose.position.z));
    const auto head = part(root, {0, 24, 0}, -std::clamp(std::remainder(pose.pitch, 360.0f), -90.0f, 90.0f),
                           -std::clamp(std::remainder(pose.head - pose.body, 360.0f), -85.0f, 85.0f));
    rlSetTexture(texture.id);
    rlBegin(RL_TRIANGLES);
    if (stand)
    {
        box(root, {-6, 0, -6}, {6, 1, 6}, 0, 32, 12, 1, 12, 0);
        box(root, {-1, 1, -1}, {1, 12, 1}, 0, 16, 2, 11, 2, 0);
        box(root, {-4, 12, -1}, {4, 14, 1}, 0, 26, 8, 2, 2, 0);
        box(root, {-6, 21, -1.5f}, {6, 24, 1.5f}, 0, 26, 12, 3, 3, 0);
        box(root, {-3, 14, -1}, {-1, 21, 1}, 16, 0, 2, 7, 2, 0);
        box(root, {1, 14, -1}, {3, 21, 1}, 16, 0, 2, 7, 2, 0);
        box(head, {-1, 0, -1}, {1, 7, 1}, 0, 0, 2, 7, 2, 0);
    }
    else
    {
        const float walk = pose.stride();
        box(head, {-4, 0, -4}, {4, 10, 4}, 0, 0, 8, 10, 8, 0);
        box(head, {-1, -1, -6}, {1, 3, -4}, 24, 0, 2, 4, 2, 0);
        if (zombie)
        {
            box(root, {-4, 12, -2}, {4, 24, 2}, 16, 16, 8, 12, 4, 0);
            box(part(root, {-6, 22, 0}, -75), {-2, -10, -2}, {2, 2, 2}, 40, 16, 4, 12, 4, 0);
            box(part(root, {6, 22, 0}, -75), {-2, -10, -2}, {2, 2, 2}, 40, 16, 4, 12, 4, 0);
        }
        else
        {
            box(head, {-4, 0, -4}, {4, 10, 4}, 32, 0, 8, 10, 8, 0.51f);
            box(root, {-4, 12, -3}, {4, 24, 3}, 16, 20, 8, 12, 6, 0);
            box(root, {-4, 4, -3}, {4, 24, 3}, 0, 38, 8, 20, 6, 0.5f);
            const auto arms = part(root, {0, 21, -1}, -43);
            box(arms, {-8, -6, -2}, {-4, 2, 2}, 44, 22, 4, 8, 4, 0);
            box(arms, {4, -6, -2}, {8, 2, 2}, 44, 22, 4, 8, 4, 0);
            box(arms, {-4, -6, -2}, {4, -2, 2}, 40, 38, 8, 4, 4, 0);
        }
        const int legV = zombie ? 16 : 22;
        box(part(root, {-2, 12, 0}, -walk), {-2, -12, -2}, {2, 0, 2}, 0, legV, 4, 12, 4, 0);
        box(part(root, {2, 12, 0}, walk), {-2, -12, -2}, {2, 0, 2}, 0, legV, 4, 12, 4, 0);
    }
    rlEnd();
    rlSetTexture(0);
}

int Avatars::name(const RemotePlayer &player, Vector3 position, Vector3 right, Vector3 up, int budget, bool draw)
{
    if (!font_.id || player.hideLabel || !player.label[0] || budget <= 0)
    {
        return 0;
    }
    struct Letter
    {
        const Glyph *glyph;
        float x;
        Color color;
        bool bold;
        unsigned row;
    };
    std::array<Letter, 64> letters;
    unsigned count = 0;
    std::array<float, 8> widths{};
    unsigned row = 0;
    Color color = WHITE;
    bool bold = false;
    const char *text = player.label.data();
    while (*text && count < letters.size() && count < static_cast<unsigned>(budget))
    {
        int bytes = 0;
        const unsigned code = static_cast<unsigned>(GetCodepointNext(text, &bytes));
        if (bytes <= 0)
        {
            break;
        }
        text += bytes;
        if (code == '\n')
        {
            if (row + 1 == widths.size())
            {
                break;
            }
            ++row;
            continue;
        }
        if (code == 0xa7 && *text)
        {
            const char format = *text++;
            const int value = hex(format);
            if (value >= 0)
            {
                color = Colors[value];
                bold = false;
            }
            else if (format == 'r')
            {
                color = WHITE;
                bold = false;
            }
            else if (format == 'l')
            {
                bold = true;
            }
            else if (format == '#' && std::strlen(text) >= 6)
            {
                unsigned rgb = 0;
                bool valid = true;
                for (int i = 0; i < 6; ++i)
                {
                    const int digit = hex(text[i]);
                    valid &= digit >= 0;
                    rgb = (rgb << 4) | static_cast<unsigned>(std::max(0, digit));
                }
                if (valid)
                {
                    color = {static_cast<unsigned char>(rgb >> 16), static_cast<unsigned char>(rgb >> 8),
                             static_cast<unsigned char>(rgb), 255};
                    text += 6;
                }
            }
            continue;
        }
        unsigned index = code < glyphIndex_.size() ? glyphIndex_[code] : glyphIndex_['?'];
        if (!index)
        {
            continue;
        }
        const auto &glyph = glyphs_[index - 1];
        letters[count++] = {&glyph, widths[row], color, bold, row};
        widths[row] += glyph.advance + (bold ? 1 : 0);
    }
    if (!count || !draw)
    {
        return static_cast<int>(count);
    }
    const float scale = 0.025f * player.labelScale;
    const auto point = [&](unsigned line, float x, float y)
    {
        return Vector3Add(position, Vector3Add(Vector3Scale(right, (x - widths[line] / 2) * scale),
                                             Vector3Scale(up, (y + (row - line) * 10) * scale)));
    };
    const auto towardCamera = Vector3Normalize(Vector3CrossProduct(right, up));
    rlSetTexture(font_.id);
    rlBegin(RL_TRIANGLES);
    for (unsigned line = 0; line <= row; ++line)
    {
        if (widths[line] <= 0)
        {
            continue;
        }
        const float edge = widths[line] + 1;
        Vector3 background[4] = {point(line, -1, -1), point(line, edge, -1), point(line, edge, 9), point(line, -1, 9)};
        for (auto &vertex : background)
        {
            vertex = Vector3Subtract(vertex, Vector3Scale(towardCamera, 0.003f));
        }
        quad(background, 255.5f / 256, 255.5f / 256, 0, 0, {0, 0, 0, 75});
    }
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &letter = letters[i];
        const auto &glyph = *letter.glyph;
        for (int pass = 0; pass < (letter.bold ? 3 : 2); ++pass)
        {
            const float x = letter.x + (pass == 0 || pass == 2 ? 1 : 0);
            const float y = pass == 0 ? -1.0f : 0.0f;
            Vector3 vertices[4] = {point(letter.row, x, y), point(letter.row, x + glyph.width, y),
                                   point(letter.row, x + glyph.width, y + 8), point(letter.row, x, y + 8)};
            const float depth = pass == 0 ? 0.001f : pass == 2 ? 0.003f : 0.002f;
            for (auto &vertex : vertices)
            {
                vertex = Vector3Add(vertex, Vector3Scale(towardCamera, depth));
            }
            const Color tint = pass == 0 ? Color{static_cast<unsigned char>(letter.color.r / 4),
                                                 static_cast<unsigned char>(letter.color.g / 4),
                                                 static_cast<unsigned char>(letter.color.b / 4), 255}
                                         : letter.color;
            quad(vertices, glyph.x / 256.0f, glyph.y / 256.0f, glyph.width / 256.0f, 8.0f / 256, tint);
        }
    }
    rlEnd();
    rlSetTexture(0);
    return static_cast<int>(count);
}
void Avatars::draw(const Session &session, Camera3D camera, Vector3 forward, float dt, const GameUi &ui)
{
    skins_.beginFrame();
    dt = std::clamp(dt, 0.0f, 0.1f);
    const float positionBlend = 1 - std::exp2(-dt / 0.075f);
    const float lookBlend = 1 - std::exp2(-dt / 0.085f);
    const float bodyBlend = 1 - std::exp2(-dt / 0.12f);
    const float attackBlend = 1 - std::exp2(-dt / 0.04f);
    const auto &players = session.players();
    const auto *cameraTarget = session.cameraTarget();
    struct Visible
    {
        unsigned index;
        float distance;
    };
    std::array<Visible, RemoteEntityLimit> visible;
    unsigned count = 0;
    for (unsigned i = 0; i < players.size(); ++i)
    {
        const auto &player = players[i];
        auto &pose = poses_[i];
        if (!player.active)
        {
            pose.id = -1;
            continue;
        }
        const Vector3 target{static_cast<float>(player.x), static_cast<float>(player.y), static_cast<float>(player.z)};
        if (pose.id != player.id || Vector3DistanceSqr(pose.position, target) > 64)
        {
            pose = {};
            pose.id = player.id;
            pose.position = target;
            pose.head = player.headYaw;
            pose.body = player.yaw;
            pose.pitch = player.pitch;
            pose.crouch = player.sneaking ? 1.0f : 0.0f;
            pose.event = player.swing;
            pose.swing = 1;
        }
        const auto before = pose.position;
        pose.position = Vector3Lerp(before, target, positionBlend);
        const float dx = pose.position.x - before.x;
        const float dz = pose.position.z - before.z;
        const float moved = std::sqrt(dx * dx + dz * dz);
        const float speed = dt > 0 ? std::min(6.0f, moved / dt) : 0;
        pose.speed += (speed - pose.speed) * bodyBlend;
        pose.phase += pose.speed * dt * 2.4f;
        if (pose.phase > 2 * Pi)
        {
            pose.phase -= 2 * Pi;
        }
        pose.body = angle(pose.body, player.yaw, bodyBlend);
        pose.head = angle(pose.head, player.headYaw, lookBlend);
        pose.pitch = angle(pose.pitch, player.pitch, lookBlend);
        pose.crouch += ((player.sneaking ? 1.0f : 0.0f) - pose.crouch) * lookBlend;
        if (pose.event != player.swing)
        {
            pose.event = player.swing;
            pose.swing = 0;
        }
        pose.swing = std::min(1.0f, pose.swing + dt / std::max(0.42f, player.swingSeconds));
        const float swing = std::sin(pose.swing * Pi);
        const float attack = swing * swing * 68;
        pose.rightAttack += ((player.offhandSwing ? 0 : attack) - pose.rightAttack) * attackBlend;
        pose.leftAttack += ((player.offhandSwing ? attack : 0) - pose.leftAttack) * attackBlend;
        const bool floatingText = player.type == remoteType::TextDisplay ||
                                  (player.type == remoteType::ArmorStand && (player.customNameVisible || player.marker));
        if (player.spectator || &player == cameraTarget || (player.invisible && !floatingText))
        {
            continue;
        }
        const auto center = floatingText ? labelPosition(player, pose.position) : Vector3Add(pose.position, {0, 1, 0});
        const auto relative = Vector3Subtract(center, camera.position);
        const float distance = Vector3LengthSqr(relative);
        if (distance > 24 * 24 || Vector3DotProduct(relative, forward) < -2)
        {
            continue;
        }
        visible[count++] = {i, distance};
    }
    std::sort(visible.begin(), visible.begin() + count,
              [](Visible a, Visible b)
              {
                  return a.distance < b.distance;
              });
#if defined(__PSP__)
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.1f);
#endif
    // One triangle stream per avatar, with no heap allocation while animating.
    for (unsigned j = 0; j < count; ++j)
    {
        const unsigned i = visible[j].index;
        const auto &player = players[i];
        if (player.invisible || player.marker || player.type == remoteType::TextDisplay ||
            player.type == remoteType::Interaction)
        {
            continue;
        }
        if (player.type == remoteType::Player || player.type == remoteType::Mannequin)
        {
            const bool slim = player.skin[0] || player.type == remoteType::Mannequin ? player.slim :
                                                                                      (player.uuid[15] & 1) != 0;
            model(player, poses_[i], skins_.texture(player.skin.data(), slim, j < 32), visible[j].distance < 12 * 12,
                  slim, &ui);
        }
        else
        {
            npcModel(player, poses_[i]);
        }
    }
    const auto right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    const auto up = Vector3Normalize(Vector3CrossProduct(right, forward));
    // rlgl batches on Windows. Submit models before changing GL depth writes,
    // then submit the complete name pass before restoring them.
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    int budget = 512;
    std::array<int, RemoteEntityLimit> labelBudgets{};
    for (unsigned j = 0; j < count && budget > 0; ++j)
    {
        const unsigned i = visible[j].index;
        const auto &player = players[i];
        const auto position = labelPosition(player, poses_[i].position);
        const int glyphs = name(player, position, right, up, budget, false);
        labelBudgets[j] = glyphs;
        budget -= glyphs;
    }
    // Alpha blended tags must be drawn back-to-front. Reserve the glyph
    // budget near-to-far first, so distant tags cannot displace nearby names.
    for (unsigned j = count; j > 0; --j)
    {
        const auto slot = j - 1;
        if (!labelBudgets[slot])
        {
            continue;
        }
        const unsigned i = visible[slot].index;
        const auto &player = players[i];
        const auto position = labelPosition(player, poses_[i].position);
        name(player, position, right, up, labelBudgets[slot]);
    }
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
#if defined(__PSP__)
    glDisable(GL_ALPHA_TEST);
#endif
}
} // namespace craft
