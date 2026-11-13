#include "render.hpp"
#include "view_volume.hpp"
#include <raymath.h>
#include <rlgl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#if defined(__PSP__)
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

namespace craft
{
namespace
{
constexpr std::size_t FacesPerMesh = 16383;
constexpr std::size_t MaxMeshBytes = 96 * 1024 * 1024;
#if defined(__PSP__)
constexpr std::size_t BytesPerFace = 108;
#else
constexpr std::size_t BytesPerFace = 156;
#endif
constexpr std::uint8_t AirFlag = 1;
constexpr std::uint8_t LiquidFlag = 2;
constexpr std::uint8_t BlendFlag = 4;
constexpr std::uint8_t WaterFlag = 8;
constexpr int Neighbor[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
constexpr int Opposite[6] = {1, 0, 3, 2, 5, 4};
struct Face
{
    std::uint32_t model;
    std::uint8_t x;
    std::uint8_t y;
    std::uint8_t z;
    std::uint8_t height;
};
#if !defined(__PSP__)
constexpr const char *VertexShader = R"(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec2 uv;
out vec4 tint;
out vec3 worldPos;
void main()
{
    worldPos = (matModel * vec4(vertexPosition, 1.0)).xyz;
    uv = vertexTexCoord;
    tint = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";
constexpr const char *FragmentShader = R"(
#version 330
in vec2 uv;
in vec4 tint;
in vec3 worldPos;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 cameraPos;
uniform float fogDistance;
out vec4 finalColor;
void main()
{
    vec4 texel = texture(texture0, uv) * tint * colDiffuse;
    if (texel.a < 0.12)
    {
        discard;
    }
    float fog = smoothstep(fogDistance * 0.475, fogDistance * 0.975, distance(cameraPos, worldPos));
    finalColor = vec4(mix(texel.rgb, vec3(0.58, 0.74, 0.82), fog), texel.a);
}
)";
#endif
} // namespace

WorldRenderer::~WorldRenderer()
{
    reset();
#if defined(__PSP__)
    if (indexBuffer_)
    {
        glDeleteBuffersARB(1, &indexBuffer_);
    }
#endif
    // Atlas materials share a shader; release that shader exactly once.
    for (unsigned i = 0; i < atlasCount_; ++i)
    {
        if (materials_[i].maps)
        {
            UnloadTexture(materials_[i].maps[MATERIAL_MAP_DIFFUSE].texture);
            MemFree(materials_[i].maps);
        }
    }
#if !defined(__PSP__)
    if (shader_.id && shader_.id != rlGetShaderIdDefault())
    {
        UnloadShader(shader_);
    }
#endif
}

void WorldRenderer::reset()
{
#if defined(__PSP__)
    cancelPspBuild(false);
    candidateCount_ = 0;
    candidateRevision_ = 0;
#endif
    for (auto &mesh : meshes_)
    {
        release(mesh);
    }
}

bool WorldRenderer::load(const char *directory)
{
    static_assert(sizeof(Block) == 16 && sizeof(ModelFace) == 40 && sizeof(Shape) == 8 && sizeof(Box) == 12,
                  "Binary model layout");
    if (loaded_ || atlasCount_)
    {
        return false;
    }
    char path[512];
    std::snprintf(path, sizeof(path), "%s/blocks.bin", directory);
    FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        return false;
    }
    char magic[4];
    std::uint32_t counts[6]{};
    bool valid =
        std::fread(magic, 1, 4, file) == 4 && std::memcmp(magic, "CCB3", 4) == 0 && std::fread(counts, 4, 6, file) == 6;
    const std::uint64_t bytes = std::uint64_t(counts[0]) * 16 + std::uint64_t(counts[1]) * 40 +
                                std::uint64_t(counts[2]) * 4 + std::uint64_t(counts[3]) * 8 +
                                std::uint64_t(counts[4]) * 12;
    valid = valid && counts[0] && counts[0] <= 65536 && counts[3] && counts[3] <= 65536 && counts[5] &&
            counts[5] <= materials_.size() && bytes <= 8 * 1024 * 1024;
    if (valid)
    {
        blocks_.resize(counts[0]);
        faces_.resize(counts[1]);
        references_.resize(counts[2]);
        shapes_.resize(counts[3]);
        boxes_.resize(counts[4]);
        valid = std::fread(blocks_.data(), sizeof(Block), blocks_.size(), file) == blocks_.size() &&
                std::fread(faces_.data(), sizeof(ModelFace), faces_.size(), file) == faces_.size() &&
                std::fread(references_.data(), 4, references_.size(), file) == references_.size() &&
                std::fread(shapes_.data(), sizeof(Shape), shapes_.size(), file) == shapes_.size() &&
                std::fread(boxes_.data(), sizeof(Box), boxes_.size(), file) == boxes_.size();
    }
    std::fclose(file);
    for (const auto &state : blocks_)
    {
        valid = valid && state.start <= references_.size() && state.count <= references_.size() - state.start &&
                state.collision < shapes_.size() && state.outline < shapes_.size();
    }
    for (const auto &shape : shapes_)
    {
        valid = valid && shape.start <= boxes_.size() && shape.count <= boxes_.size() - shape.start;
    }
    for (auto reference : references_)
    {
        valid = valid && reference < faces_.size();
    }
    for (const auto &face : faces_)
    {
        valid = valid && face.tile < counts[5] * 1024 && face.cull >= -1 && face.cull < 6;
    }
    if (!valid)
    {
        std::vector<Block>().swap(blocks_);
        std::vector<ModelFace>().swap(faces_);
        std::vector<std::uint32_t>().swap(references_);
        std::vector<Shape>().swap(shapes_);
        std::vector<Box>().swap(boxes_);
        return false;
    }
#if !defined(__PSP__)
    shader_ = LoadShaderFromMemory(VertexShader, FragmentShader);
    if (IsShaderValid(shader_))
    {
        cameraLocation_ = GetShaderLocation(shader_, "cameraPos");
        fogDistanceLocation_ = GetShaderLocation(shader_, "fogDistance");
    }
#endif
    for (unsigned i = 0; i < counts[5]; ++i)
    {
        if (i == 0)
        {
            std::snprintf(path, sizeof(path), "%s/blocks.png", directory);
        }
        else
        {
            std::snprintf(path, sizeof(path), "%s/blocks%u.png", directory, i);
        }
#if defined(__PSP__)
        Image image = LoadImage(path);
        if (!image.data)
        {
            return false;
        }
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R4G4B4A4);
        Texture2D texture = LoadTextureFromImage(image);
        UnloadImage(image);
#else
        Texture2D texture = LoadTexture(path);
#endif
        if (!texture.id)
        {
            return false;
        }
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
        auto material = LoadMaterialDefault();
        if (!material.maps)
        {
            UnloadTexture(texture);
            return false;
        }
        material.maps[MATERIAL_MAP_DIFFUSE].texture = texture;
#if !defined(__PSP__)
        if (IsShaderValid(shader_))
        {
            material.shader = shader_;
        }
#endif
        materials_[atlasCount_++] = material;
    }
#if defined(__PSP__)
    if (!initializePsp())
    {
        return false;
    }
#endif
    loaded_ = true;
    return true;
}

const WorldRenderer::Block &WorldRenderer::block(BlockState id) const
{
    static const Block fallback{0, 0, 1, 0, 63, 0, 0, 1, 0};
    return id < blocks_.size() ? blocks_[id] : fallback;
}

bool WorldRenderer::isAir(BlockState id) const
{
    return (block(id).flags & AirFlag) != 0;
}

bool WorldRenderer::isSolid(BlockState id) const
{
    return block(id).collision != 0;
}

bool WorldRenderer::isLiquid(BlockState id) const
{
    return (block(id).flags & LiquidFlag) != 0;
}

bool WorldRenderer::intersects(BlockState id, float x0, float y0, float z0, float x1, float y1, float z1) const
{
    if (shapes_.empty())
    {
        return isSolid(id);
    }
    const auto &shape = shapes_[block(id).collision];
    for (std::uint32_t i = 0; i < shape.count; ++i)
    {
        const auto &b = boxes_[shape.start + i].bounds;
        if (x1 > b[0] * (1.0f / 256) && y1 > b[1] * (1.0f / 256) && z1 > b[2] * (1.0f / 256) &&
            x0 < b[3] * (1.0f / 256) && y0 < b[4] * (1.0f / 256) && z0 < b[5] * (1.0f / 256))
        {
            return true;
        }
    }
    return false;
}

double WorldRenderer::clipAxis(BlockState id, int axis, const double *low, const double *high, double delta) const
{
    const auto &shape = shapes_[block(id).collision];
    const int a = (axis + 1) % 3;
    const int c = (axis + 2) % 3;
    for (std::uint32_t i = 0; i < shape.count; ++i)
    {
        const auto &bounds = boxes_[shape.start + i].bounds;
        if (high[a] <= bounds[a] / 256.0 || low[a] >= bounds[a + 3] / 256.0 || high[c] <= bounds[c] / 256.0 ||
            low[c] >= bounds[c + 3] / 256.0)
        {
            continue;
        }
        const double min = bounds[axis] / 256.0;
        const double max = bounds[axis + 3] / 256.0;
        if (delta > 0 && high[axis] <= min)
        {
            delta = std::min(delta, min - high[axis]);
        }
        else if (delta < 0 && low[axis] >= max)
        {
            delta = std::max(delta, max - low[axis]);
        }
    }
    return delta;
}

bool WorldRenderer::intersectRay(BlockState id, Vector3 origin, Vector3 direction, float limit, float &distance,
                                 int &face) const
{
    if (isAir(id) || isLiquid(id) || shapes_.empty())
    {
        return false;
    }
    const auto &shape = shapes_[block(id).outline];
    bool hit = false;
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {direction.x, direction.y, direction.z};
    constexpr int lowFace[3] = {4, 0, 2};
    constexpr int highFace[3] = {5, 1, 3};
    for (std::uint32_t i = 0; i < shape.count; ++i)
    {
        const auto &b = boxes_[shape.start + i].bounds;
        float near = 0;
        float far = limit;
        int side = 1;
        for (int axis = 0; axis < 3 && far >= near; ++axis)
        {
            const float low = b[axis] / 256.0f;
            const float high = std::max(low + 0.002f, b[axis + 3] / 256.0f);
            if (std::abs(d[axis]) < 0.000001f)
            {
                if (o[axis] < low || o[axis] > high)
                {
                    far = -1;
                }
                continue;
            }
            float a = (low - o[axis]) / d[axis];
            float c = (high - o[axis]) / d[axis];
            const int entered = d[axis] > 0 ? lowFace[axis] : highFace[axis];
            if (a > c)
            {
                std::swap(a, c);
            }
            if (a > near)
            {
                near = a;
                side = entered;
            }
            far = std::min(far, c);
        }
        if (far >= near && near <= limit)
        {
            limit = near;
            distance = near;
            face = side;
            hit = true;
        }
    }
    return hit;
}

void WorldRenderer::drawOutline(BlockState id, int x, int y, int z) const
{
    if (shapes_.empty())
    {
        return;
    }
    const auto &shape = shapes_[block(id).outline];
    for (std::uint32_t i = 0; i < shape.count; ++i)
    {
        const auto &b = boxes_[shape.start + i].bounds;
        DrawCubeWires({x + (b[0] + b[3]) / 512.0f, y + (b[1] + b[4]) / 512.0f, z + (b[2] + b[5]) / 512.0f},
                      (b[3] - b[0]) / 256.0f + 0.004f, (b[4] - b[1]) / 256.0f + 0.004f, (b[5] - b[2]) / 256.0f + 0.004f,
                      BLACK);
    }
}

void WorldRenderer::drawCracks(BlockState id, int x, int y, int z, Texture2D atlas, Rectangle tile) const
{
    if (!atlas.id || !loaded_ || isAir(id) || isLiquid(id))
    {
        return;
    }
    const auto &model = block(id);
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    rlSetTexture(atlas.id);
    rlBegin(RL_TRIANGLES);
    rlColor4ub(255, 255, 255, 225);
    for (unsigned i = 0; i < model.count; ++i)
    {
        const auto &face = faces_[references_[model.start + i] & 0xffffff];
        Vector3 vertices[4];
        for (int k = 0; k < 4; ++k)
        {
            vertices[k] = {x + face.position[k * 3] / 256.0f, y + face.position[k * 3 + 1] / 256.0f,
                           z + face.position[k * 3 + 2] / 256.0f};
        }
        const auto offset = Vector3Scale(Vector3Normalize(Vector3CrossProduct(Vector3Subtract(vertices[1], vertices[0]),
                                                                             Vector3Subtract(vertices[2], vertices[0]))),
                                         0.002f);
        for (const int k : {0, 1, 2, 0, 2, 3})
        {
            const auto vertex = Vector3Add(vertices[k], offset);
            const float u = (tile.x + 0.5f + face.uv[k * 2] * (tile.width - 1) / 255.0f) / atlas.width;
            const float v = (tile.y + 0.5f + face.uv[k * 2 + 1] * (tile.height - 1) / 255.0f) / atlas.height;
            rlTexCoord2f(u, v);
            rlVertex3f(vertex.x, vertex.y, vertex.z);
        }
    }
    rlEnd();
    rlDrawRenderBatchActive();
    rlSetTexture(0);
    rlEnableDepthMask();
}

float WorldRenderer::waterHeight(const World &world, int x, int y, int z) const
{
    BlockState state;
    if (!world.get(x, y, z, state) || (block(state).flags & WaterFlag) == 0)
    {
        return 0;
    }
    BlockState above;
    if (world.get(x, y + 1, z, above) && (block(above).flags & WaterFlag) != 0)
    {
        return 1;
    }
    return block(state).height / 255.0f;
}

void WorldRenderer::release(Cached &cache)
{
    if (!cache.used)
    {
        return;
    }
#if defined(__PSP__)
    if (cache.buffer)
    {
        glDeleteBuffersARB(1, &cache.buffer);
    }
#else
    for (std::size_t i = 0; i < cache.meshCount; ++i)
    {
        UnloadMesh(cache.meshes[i]);
    }
#endif
    meshBytes_ -= cache.bytes;
    if (cache.bytes)
    {
        ++budgetEpoch_;
    }
    cache = Cached{};
}

bool WorldRenderer::hiddenUniformSection(const World &world, SectionPos pos, BlockState state) const
{
    const auto &model = block(state);
    if (!model.count)
    {
        return true;
    }
    if (model.occlusion != 63)
    {
        return false;
    }
    for (int side = 0; side < 6; ++side)
    {
        const auto &offset = Neighbor[side];
        BlockState adjacent;
        const World::Blocks *blocks;
        std::uint64_t version;
        if (!world.section({pos.x + offset[0], pos.y + offset[1], pos.z + offset[2]}, adjacent, blocks, version) ||
            blocks || !(block(adjacent).occlusion & (1 << Opposite[side])))
        {
            return false;
        }
    }
    for (unsigned i = 0; i < model.count; ++i)
    {
        if (faces_[references_[model.start + i]].cull < 0)
        {
            return false;
        }
    }
    return true;
}

#if !defined(__PSP__)
bool WorldRenderer::build(Cached &cache, const World &world, SectionPos pos, BlockState uniform,
                          const World::Blocks *blocks, std::uint64_t version)
{
    std::vector<Face> faces;
    faces.reserve(512);
    const std::size_t available = (MaxMeshBytes - meshBytes_) / BytesPerFace;
    const int ox = pos.x * 16;
    const int oy = pos.y * 16;
    const int oz = pos.z * 16;
    std::array<std::size_t, 8> counts{};
    for (int y = 0; y < 16; ++y)
    {
        for (int z = 0; z < 16; ++z)
        {
            for (int x = 0; x < 16; ++x)
            {
                const auto current = blocks ? (*blocks)[(y * 16 + z) * 16 + x] : uniform;
                const auto &state = block(current);
                if (!state.count)
                {
                    continue;
                }
                std::uint8_t hidden = 0;
                std::uint8_t fluidHidden = 0;
                std::uint8_t height = state.height;
                const int liquid = (state.flags & WaterFlag) ? 1 : state.liquid;
                for (int side = 0; side < 6; ++side)
                {
                    const int nx = x + Neighbor[side][0];
                    const int ny = y + Neighbor[side][1];
                    const int nz = z + Neighbor[side][2];
                    BlockState other;
                    const bool inside = nx >= 0 && nx < 16 && ny >= 0 && ny < 16 && nz >= 0 && nz < 16;
                    const bool known = inside ? (other = blocks ? (*blocks)[(ny * 16 + nz) * 16 + nx] : uniform, true)
                                              : world.get(ox + nx, oy + ny, oz + nz, other);
                    if (!known)
                    {
                        continue;
                    }
                    const auto &adjacent = block(other);
                    const int adjacentLiquid = (adjacent.flags & WaterFlag) ? 1 : adjacent.liquid;
                    const bool covered = (adjacent.occlusion & (1 << Opposite[side])) != 0;
                    if (liquid && side == 4 && liquid == adjacentLiquid)
                    {
                        height = 255;
                    }
                    if (covered || ((state.flags & 16) && (adjacent.flags & 16) && state.group == adjacent.group))
                    {
                        hidden |= 1 << side;
                    }
                    if (covered || (liquid && liquid == adjacentLiquid &&
                                    (side == 4 || side == 5 || adjacent.height >= state.height)))
                    {
                        fluidHidden |= 1 << side;
                    }
                }
                for (std::uint32_t i = 0; i < state.count; ++i)
                {
                    const auto id = references_[state.start + i];
                    const auto &face = faces_[id];
                    const bool fluidFace = (state.flags & LiquidFlag) || face.tint == 3;
                    const auto mask = fluidFace ? fluidHidden : hidden;
                    if (face.cull >= 0 && (mask & (1 << face.cull)))
                    {
                        continue;
                    }
                    if (faces.size() >= available)
                    {
                        return false;
                    }
                    const unsigned layer =
                        (face.tile / 1024) * 2 + (((state.flags & BlendFlag) || face.tint == 3) ? 1 : 0);
                    ++counts[layer];
                    faces.push_back({id | (layer << 24), static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y),
                                     static_cast<std::uint8_t>(z),
                                     static_cast<std::uint8_t>(fluidFace ? height : 255)});
                }
            }
        }
    }
    std::sort(faces.begin(), faces.end(),
              [](const Face &a, const Face &b)
              {
                  return (a.model >> 24) < (b.model >> 24);
              });
    cache.used = true;
    cache.pos = pos;
    cache.version = version;
    std::size_t start = 0;
    for (unsigned layer = 0; layer < counts.size(); ++layer)
    {
        for (std::size_t remaining = counts[layer]; remaining;)
        {
            if (cache.meshCount == cache.meshes.size())
            {
                release(cache);
                return false;
            }
            const auto amount = std::min(remaining, FacesPerMesh);
            auto &mesh = cache.meshes[cache.meshCount];
            mesh.vertexCount = static_cast<int>(amount * 4);
            mesh.triangleCount = static_cast<int>(amount * 2);
            mesh.vertices = static_cast<float *>(MemAlloc(static_cast<unsigned>(amount * 48)));
            mesh.texcoords = static_cast<float *>(MemAlloc(static_cast<unsigned>(amount * 32)));
            mesh.colors = static_cast<unsigned char *>(MemAlloc(static_cast<unsigned>(amount * 16)));
            mesh.indices = static_cast<unsigned short *>(MemAlloc(static_cast<unsigned>(amount * 12)));
#if !defined(__PSP__)
            mesh.normals = static_cast<float *>(MemAlloc(static_cast<unsigned>(amount * 48)));
#endif
            if (!mesh.vertices || !mesh.texcoords || !mesh.colors || !mesh.indices
#if !defined(__PSP__)
                || !mesh.normals
#endif
            )
            {
                MemFree(mesh.vertices);
                MemFree(mesh.texcoords);
                MemFree(mesh.colors);
                MemFree(mesh.indices);
                MemFree(mesh.normals);
                mesh = {};
                release(cache);
                return false;
            }
            for (std::size_t i = 0; i < amount; ++i)
            {
                const auto &placed = faces[start + i];
                const auto &f = faces_[placed.model & 0xffffff];
                const unsigned tile = f.tile % 1024;
                const float u = (tile % 32) / 32.0f;
                const float v = (tile / 32) / 32.0f;
                const float shade = f.shade ? 1.0f
                                            : 0.76f + 0.24f * std::max(0.0f, f.normal[1] / 127.0f) -
                                                  0.12f * std::abs(f.normal[0] / 127.0f);
                const Color tint = f.tint == 1   ? Color{126, 189, 82, 255}
                                   : f.tint == 2 ? Color{190, 32, 20, 255}
                                   : f.tint == 3 ? Color{72, 112, 220, 255}
                                                 : WHITE;
                for (int k = 0; k < 4; ++k)
                {
                    const auto at = i * 4 + k;
                    mesh.vertices[at * 3] = placed.x + f.position[k * 3] / 256.0f;
                    mesh.vertices[at * 3 + 1] = placed.y + f.position[k * 3 + 1] / 256.0f * (placed.height / 255.0f);
                    mesh.vertices[at * 3 + 2] = placed.z + f.position[k * 3 + 2] / 256.0f;
                    constexpr float inset = 1.0f / 8192;
                    constexpr float span = 1.0f / 32 - inset * 2;
                    mesh.texcoords[at * 2] = u + inset + f.uv[k * 2] / 255.0f * span;
                    mesh.texcoords[at * 2 + 1] = v + inset + f.uv[k * 2 + 1] / 255.0f * span;
                    mesh.colors[at * 4] = static_cast<unsigned char>(tint.r * shade);
                    mesh.colors[at * 4 + 1] = static_cast<unsigned char>(tint.g * shade);
                    mesh.colors[at * 4 + 2] = static_cast<unsigned char>(tint.b * shade);
                    mesh.colors[at * 4 + 3] = 255;
#if !defined(__PSP__)
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        mesh.normals[at * 3 + axis] = f.normal[axis] / 127.0f;
                    }
#endif
                }
                const auto base = static_cast<unsigned short>(i * 4);
                const unsigned short indices[6] = {
                    base, static_cast<unsigned short>(base + 1), static_cast<unsigned short>(base + 2),
                    base, static_cast<unsigned short>(base + 2), static_cast<unsigned short>(base + 3)};
                std::memcpy(mesh.indices + i * 6, indices, sizeof(indices));
            }
            UploadMesh(&mesh, false);
            cache.layers[cache.meshCount++] = static_cast<std::uint8_t>(layer);
            cache.bytes += amount * BytesPerFace;
            meshBytes_ += amount * BytesPerFace;
            start += amount;
            remaining -= amount;
        }
    }
    return true;
}
#endif

void WorldRenderer::draw(const World &world, Vector3 camera, Vector3 forward, float fovY, float viewDistance)
{
#if defined(__PSP__)
    drawPsp(world, camera, forward, fovY, viewDistance);
#else
    if (!loaded_)
    {
        return;
    }
    rlDrawRenderBatchActive();
#if defined(__PSP__)
    const GLfloat fogColor[4] = {0.58f, 0.74f, 0.82f, 1};
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 38);
    glFogf(GL_FOG_END, 78);
    glFogfv(GL_FOG_COLOR, fogColor);
    // Discard transparent texels before they can write depth (leaves and flowers).
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.12f);
#else
    if (cameraLocation_ >= 0)
    {
        const float p[3] = {camera.x, camera.y, camera.z};
        SetShaderValue(shader_, cameraLocation_, p, SHADER_UNIFORM_VEC3);
    }
    if (fogDistanceLocation_ >= 0)
    {
        SetShaderValue(shader_, fogDistanceLocation_, &viewDistance, SHADER_UNIFORM_FLOAT);
    }
#endif
    const ViewVolume volume(forward, fovY);
    struct Candidate
    {
        SectionPos pos;
        BlockState uniform;
        const World::Blocks *blocks;
        std::uint64_t version;
        float distance;
        Cached *mesh = nullptr;
        bool visible = false;
    };
    std::array<Candidate, World::MaxSections> candidates{};
    std::size_t count = 0;
    world.forEach(
        [&](SectionPos pos, BlockState uniform, const World::Blocks *blocks, std::uint64_t version)
        {
            const float dx = pos.x * 16.0f + 8 - camera.x;
            const float dy = pos.y * 16.0f + 8 - camera.y;
            const float dz = pos.z * 16.0f + 8 - camera.z;
            const float distance = sectionDistanceSquared(dx, dy, dz);
            if (distance <= viewDistance * viewDistance && count < candidates.size() &&
                (blocks || !hiddenUniformSection(world, pos, uniform)))
            {
                candidates[count++] = {pos, uniform, blocks, version, distance, nullptr, volume.contains(dx, dy, dz)};
            }
        });
    const auto selected = std::min(count, meshes_.size());
    std::partial_sort(candidates.begin(), candidates.begin() + selected, candidates.begin() + count,
              [](const Candidate &a, const Candidate &b)
              {
                  return a.visible != b.visible ? a.visible : a.distance < b.distance;
              });
    count = selected;
    for (auto &mesh : meshes_)
    {
        if (!mesh.used)
        {
            continue;
        }
        bool wanted = false;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (mesh.pos == candidates[i].pos)
            {
                wanted = true;
                break;
            }
        }
        if (!wanted)
        {
            release(mesh);
        }
    }
    Candidate *pending = nullptr;
    Cached *target = nullptr;
    const double now = GetTime();
    for (std::size_t i = 0; i < count; ++i)
    {
        auto &candidate = candidates[i];
        Cached *found = nullptr;
        Cached *unused = nullptr;
        for (auto &mesh : meshes_)
        {
            if (mesh.used && mesh.pos == candidate.pos)
            {
                found = &mesh;
            }
            else if (!mesh.used && !unused)
            {
                unused = &mesh;
            }
        }
        const bool failed = found && found->blocked && found->failedVersion == candidate.version &&
                            found->failureEpoch == budgetEpoch_ && now < found->retryAfter;
        candidate.mesh = found;
        // Give gaps priority over refreshing an already drawable section.
        const bool missing = !found || !found->version;
        if (candidate.visible && (found || unused) && (!found || found->version != candidate.version) && !failed &&
            (!pending || (missing && target->version)))
        {
            pending = &candidate;
            target = found ? found : unused;
        }
    }
    if (pending)
    {
        Cached replacement;
        if (build(replacement, world, pending->pos, pending->uniform, pending->blocks, pending->version))
        {
            release(*target);
            *target = replacement;
        }
        else
        {
            release(replacement);
            target->pos = pending->pos;
            target->used = true;
            target->blocked = true;
            target->failedVersion = pending->version;
            target->failureEpoch = budgetEpoch_;
            target->retryAfter = now + 1.0;
        }
        pending->mesh = target;
    }
    // Opaque/cutout first, translucent sections from far to near without depth writes.
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
        {
            rlDisableDepthMask();
            rlDisableBackfaceCulling();
        }
        for (std::size_t j = 0; j < count; ++j)
        {
            const auto &c = candidates[pass == 0 ? j : count - 1 - j];
            if (!c.mesh || !c.visible)
            {
                continue;
            }
            for (std::size_t part = 0; part < c.mesh->meshCount; ++part)
            {
                const unsigned layer = c.mesh->layers[part];
                if (static_cast<int>(layer % 2) == pass)
                {
                    DrawMesh(c.mesh->meshes[part], materials_[layer / 2],
                             MatrixTranslate(c.pos.x * 16.0f, c.pos.y * 16.0f, c.pos.z * 16.0f));
                }
            }
        }
    }
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
#if defined(__PSP__)
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
#endif
#endif
}
} // namespace craft
