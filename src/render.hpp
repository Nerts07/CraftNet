#pragma once
#include "world.hpp"
#include <raylib.h>
#include <array>
#include <cstdint>
#include <vector>

namespace craft
{
class WorldRenderer final
{
  public:
    WorldRenderer() = default;
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer &) = delete;
    WorldRenderer &operator=(const WorldRenderer &) = delete;
    bool load(const char *directory);
    void draw(const World &world, Vector3 camera, Vector3 forward, float fovY, float viewDistance);
    void reset();
    bool isAir(BlockState id) const;
    bool isSolid(BlockState id) const;
    bool isLiquid(BlockState id) const;
    bool intersects(BlockState id, float x0, float y0, float z0, float x1, float y1, float z1) const;
    double clipAxis(BlockState id, int axis, const double *low, const double *high, double delta) const;
    bool intersectRay(BlockState id, Vector3 origin, Vector3 direction, float limit, float &distance, int &face) const;
    void drawOutline(BlockState id, int x, int y, int z) const;
    void drawCracks(BlockState id, int x, int y, int z, Texture2D atlas, Rectangle tile) const;
    float waterHeight(const World &world, int x, int y, int z) const;
    std::size_t meshBytes() const
    {
        return meshBytes_;
    }

  private:
    struct Block
    {
        std::uint32_t start = 0;
        std::uint16_t count = 0;
        std::uint16_t collision = 0;
        std::uint8_t flags = 1;
        std::uint8_t occlusion = 0;
        std::uint8_t height = 0;
        std::uint8_t liquid = 0;
        std::uint16_t outline = 0;
        std::uint16_t group = 0;
    };
    struct ModelFace
    {
        std::int16_t position[12];
        std::uint8_t uv[8];
        std::uint16_t tile;
        union
        {
            std::int8_t normal[3];
            std::uint8_t color[3];
        };
        std::int8_t cull;
        std::uint8_t tint;
        std::uint8_t shade;
    };
    struct Shape
    {
        std::uint32_t start;
        std::uint32_t count;
    };
    struct Box
    {
        std::int16_t bounds[6];
    };
    struct Cached
    {
        SectionPos pos{};
        std::uint64_t version = 0;
        std::uint64_t failureEpoch = 0;
        std::uint64_t failedVersion = 0;
        double retryAfter = 0;
#if defined(__PSP__)
        unsigned buffer = 0;
        struct Batch
        {
            std::uint32_t first = 0;
            std::uint16_t faces = 0;
            std::uint8_t layer = 0;
            std::uint8_t direction = 6;
        };
        std::array<Batch, 64> batches{};
#else
        std::array<Mesh, 12> meshes{};
        std::array<std::uint8_t, 12> layers{};
#endif
        std::size_t meshCount = 0;
        std::size_t bytes = 0;
        bool used = false;
        bool blocked = false;
    };
    const Block &block(BlockState id) const;
    bool hiddenUniformSection(const World &world, SectionPos pos, BlockState state) const;
    void release(Cached &cached);
#if defined(__PSP__)
    static constexpr std::size_t PspSectionFaces = 49152;
    struct PendingFace
    {
        std::uint32_t model;
        std::uint8_t x, y, z, height;
        std::uint16_t next;
    };
    struct Candidate
    {
        SectionPos pos{};
        std::uint64_t version = 0;
        float distance = 0;
        Cached *mesh = nullptr;
        bool visible = false;
        bool nearby = false;
    };
    struct BuildJob
    {
        std::array<BlockState, 18 * 18 * 18> snapshot{};
        std::unique_ptr<PendingFace[]> faces;
        std::array<std::uint16_t, 56> heads{}, tails{}, counts{};
        Cached result{};
        Cached *target = nullptr;
        std::size_t faceCount = 0;
        unsigned writtenFaces = 0;
        unsigned blockIndex = 0, layer = 0;
        std::uint16_t cursor = 65535;
        bool active = false;
    };
    bool initializePsp();
    void beginPspBuild(Cached &target, const World &world, SectionPos pos, std::uint64_t version);
    void advancePspBuild();
    void cancelPspBuild(bool failed);
    void drawPsp(const World &world, Vector3 camera, Vector3 forward, float fovY, float viewDistance);
    BuildJob job_;
    std::array<Candidate, World::MaxSections> candidates_{};
    std::size_t candidateCount_ = 0;
    std::uint64_t candidateRevision_ = 0;
    SectionPos candidateCell_{};
    unsigned indexBuffer_ = 0;
#else
    bool build(Cached &cached, const World &world, SectionPos pos, BlockState uniform, const World::Blocks *blocks,
               std::uint64_t version);
#endif
    std::vector<Block> blocks_;
    std::vector<ModelFace> faces_;
    std::vector<std::uint32_t> references_;
    std::vector<Shape> shapes_;
    std::vector<Box> boxes_;
#if defined(__PSP__)
    std::array<Cached, 160> meshes_{};
#else
    std::array<Cached, 256> meshes_{};
#endif
    std::array<Material, 4> materials_{};
    Shader shader_{};
    unsigned atlasCount_ = 0;
    bool loaded_ = false;
    int cameraLocation_ = -1, fogDistanceLocation_ = -1;
    std::size_t meshBytes_ = 0;
    std::uint64_t budgetEpoch_ = 0;
};
} // namespace craft
