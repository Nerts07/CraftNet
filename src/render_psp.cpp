#include "render.hpp"
#include "view_volume.hpp"
#if defined(__PSP__)
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <pspkernel.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>

namespace craft
{
namespace
{
constexpr std::size_t MeshBudget = 12 * 1024 * 1024;
// GE's primitive count is 16 bits: six indices per quad must fit too.
constexpr unsigned FacesPerBatch = 10922;
constexpr unsigned FacesPerUpload = 512;
static_assert(FacesPerBatch * 6 <= 65535 && FacesPerBatch * 4 <= 65536, "GE draw/index limits");
constexpr std::uint16_t End = 65535;
// Include the section bounds and movement inside a 4-block scheduling cell.
constexpr int Offsets[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
constexpr int Opposite[6] = {1, 0, 3, 2, 5, 4};

// PSPGL's native layout: texture coordinates, RGBA8888, then position.
// A single VBO in this layout bypasses per-draw vertex conversion.
struct Vertex
{
    std::int16_t u, v;
    std::uint32_t color;
    float x, y, z;
};
static_assert(sizeof(Vertex) == 20 && offsetof(Vertex, color) == 4 && offsetof(Vertex, x) == 8,
              "PSPGL native vertex layout");

} // namespace

bool WorldRenderer::initializePsp()
{
    job_.faces.reset(new (std::nothrow) PendingFace[PspSectionFaces]);
    if (!job_.faces)
    {
        return false;
    }
    // Bake face lighting once. The binary normal bytes are no longer needed on PSP.
    for (auto &face : faces_)
    {
        unsigned direction = 6;
        for (unsigned side = 0; side < 6; ++side)
        {
            if (face.normal[0] == Offsets[side][0] * 127 && face.normal[1] == Offsets[side][1] * 127 &&
                face.normal[2] == Offsets[side][2] * 127)
            {
                direction = side;
                break;
            }
        }
        const float shade = face.shade ? 1.0f
                                       : 0.76f + 0.24f * std::max(0.0f, face.normal[1] / 127.0f) -
                                             0.12f * std::abs(face.normal[0] / 127.0f);
        const Color tint = face.tint == 1   ? Color{126, 189, 82, 255}
                           : face.tint == 2 ? Color{190, 32, 20, 255}
                           : face.tint == 3 ? Color{72, 112, 220, 255}
                                            : WHITE;
        face.color[0] = static_cast<unsigned char>(tint.r * shade);
        face.color[1] = static_cast<unsigned char>(tint.g * shade);
        face.color[2] = static_cast<unsigned char>(tint.b * shade);
        face.shade = static_cast<std::uint8_t>(direction);
    }
    for (auto &state : blocks_)
    {
        bool allCullable = true;
        for (unsigned i = 0; i < state.count; ++i)
        {
            if (faces_[references_[state.start + i]].cull < 0)
            {
                allCullable = false;
                break;
            }
        }
        if (allCullable)
        {
            state.flags |= 32;
        }
    }
    glGenBuffersARB(1, &indexBuffer_);
    glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, indexBuffer_);
    // Keep geometry in system RAM; leave scarce EDRAM for textures/framebuffers.
    glBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, FacesPerBatch * 12, nullptr, GL_DYNAMIC_DRAW_ARB);
    auto *indices = static_cast<unsigned short *>(glMapBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, GL_READ_WRITE_ARB));
    if (!indices)
    {
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        return false;
    }
    for (unsigned i = 0; i < FacesPerBatch; ++i)
    {
        const unsigned short base = static_cast<unsigned short>(i * 4);
        indices[i * 6] = base;
        indices[i * 6 + 1] = base + 1;
        indices[i * 6 + 2] = base + 2;
        indices[i * 6 + 3] = base;
        indices[i * 6 + 4] = base + 2;
        indices[i * 6 + 5] = base + 3;
    }
    const bool valid = glUnmapBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB) == GL_TRUE;
    glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
    return valid;
}

void WorldRenderer::cancelPspBuild(bool failed)
{
    if (!job_.active)
    {
        return;
    }
    const auto version = job_.result.version;
    // Discarding scratch geometry does not create more room for resident meshes.
    const auto epoch = budgetEpoch_;
    release(job_.result);
    budgetEpoch_ = epoch;
    if (failed && job_.target)
    {
        job_.target->blocked = true;
        job_.target->failedVersion = version;
        job_.target->failureEpoch = budgetEpoch_;
        job_.target->retryAfter = GetTime() + 2.0;
    }
    job_.active = false;
    job_.target = nullptr;
}

void WorldRenderer::beginPspBuild(Cached &target, const World &world, SectionPos pos, std::uint64_t version)
{
    job_.active = true;
    job_.target = &target;
    job_.faceCount = 0;
    job_.writtenFaces = 0;
    job_.blockIndex = 0;
    job_.layer = 0;
    job_.cursor = End;
    job_.heads.fill(End);
    job_.tails.fill(End);
    job_.counts.fill(0);
    job_.result = {};
    job_.result.used = true;
    job_.result.pos = pos;
    job_.result.version = version;
    if (!target.used)
    {
        target.used = true;
        target.pos = pos;
    }
    // Own a small snapshot: chunk updates between frames cannot invalidate a build.
    job_.snapshot.fill(0);
    BlockState uniform = 0;
    const World::Blocks *data = nullptr;
    std::uint64_t ignored;
    if (!world.section(pos, uniform, data, ignored))
    {
        cancelPspBuild(false);
        return;
    }
    for (int y = 0; y < 16; ++y)
    {
        for (int z = 0; z < 16; ++z)
        {
            auto *row = job_.snapshot.data() + (y + 1) * 324 + (z + 1) * 18 + 1;
            if (data)
            {
                std::copy_n(data->data() + y * 256 + z * 16, 16, row);
            }
            else
            {
                std::fill_n(row, 16, uniform);
            }
        }
    }
    // Resolve each neighbouring section once, rather than hashing every boundary block.
    for (int side = 0; side < 6; ++side)
    {
        const auto &d = Offsets[side];
        if (!world.section({pos.x + d[0], pos.y + d[1], pos.z + d[2]}, uniform, data, ignored))
        {
            continue;
        }
        for (int a = 0; a < 16; ++a)
        {
            for (int b = 0; b < 16; ++b)
            {
                const int x = side == 2 ? 0 : side == 3 ? 15 : a;
                const int y = side == 4 ? 15 : side == 5 ? 0 : b;
                const int z = side == 0 ? 0 : side == 1 ? 15 : side < 4 ? a : b;
                const int source = (y - d[1] * 15) * 256 + (z - d[2] * 15) * 16 + x - d[0] * 15;
                const int destination = (y + 1 + d[1]) * 324 + (z + 1 + d[2]) * 18 + x + 1 + d[0];
                job_.snapshot[destination] = data ? (*data)[source] : uniform;
            }
        }
    }
}

void WorldRenderer::advancePspBuild()
{
    if (!job_.active)
    {
        return;
    }
    if (job_.blockIndex < 4096)
    {
        const auto started = sceKernelGetSystemTimeLow();
        const unsigned end = std::min(4096u, job_.blockIndex + 1024);
        while (job_.blockIndex < end)
        {
            if ((job_.blockIndex & 31) == 0 && sceKernelGetSystemTimeLow() - started >= 1000)
            {
                break;
            }
            const unsigned at = job_.blockIndex++;
            const unsigned x = at & 15;
            const unsigned z = (at >> 4) & 15;
            const unsigned y = at >> 8;
            const int index = (y + 1) * 324 + (z + 1) * 18 + x + 1;
            const auto &state = block(job_.snapshot[index]);
            if (!state.count)
            {
                continue;
            }
            unsigned hidden = 0;
            unsigned fluidHidden = 0;
            unsigned height = state.height;
            const int liquid = (state.flags & 8) ? 1 : state.liquid;
            constexpr int stride[6] = {-18, 18, -1, 1, 324, -324};
            for (int side = 0; side < 6; ++side)
            {
                const auto &adjacent = block(job_.snapshot[index + stride[side]]);
                const int adjacentLiquid = (adjacent.flags & 8) ? 1 : adjacent.liquid;
                const bool covered = (adjacent.occlusion & (1 << Opposite[side])) != 0;
                if (liquid && side == 4 && liquid == adjacentLiquid)
                {
                    height = 255;
                }
                if (covered || ((state.flags & 16) && (adjacent.flags & 16) && state.group == adjacent.group))
                {
                    hidden |= 1 << side;
                }
                if (covered ||
                    (liquid && liquid == adjacentLiquid && (side == 4 || side == 5 || adjacent.height >= state.height)))
                {
                    fluidHidden |= 1 << side;
                }
            }
            if ((state.flags & 32) && hidden == 63 && fluidHidden == 63)
            {
                continue;
            }
            for (unsigned i = 0; i < state.count; ++i)
            {
                const auto id = references_[state.start + i];
                const auto &face = faces_[id];
                const bool fluid = (state.flags & 2) || face.tint == 3;
                if (face.cull >= 0 && ((fluid ? fluidHidden : hidden) & (1 << face.cull)))
                {
                    continue;
                }
                if (job_.faceCount == PspSectionFaces)
                {
                    cancelPspBuild(true);
                    return;
                }
                const unsigned material = (face.tile / 1024) * 2 + (((state.flags & 4) || face.tint == 3) ? 1 : 0);
                const unsigned layer = material * 7 + face.shade;
                const auto next = static_cast<std::uint16_t>(job_.faceCount++);
                job_.faces[next] = {id,
                                    static_cast<std::uint8_t>(x),
                                    static_cast<std::uint8_t>(y),
                                    static_cast<std::uint8_t>(z),
                                    static_cast<std::uint8_t>(fluid ? height : 255),
                                    End};
                if (job_.tails[layer] == End)
                {
                    job_.heads[layer] = next;
                }
                else
                {
                    job_.faces[job_.tails[layer]].next = next;
                }
                job_.tails[layer] = next;
                ++job_.counts[layer];
            }
        }
        return;
    }
    while (job_.layer < atlasCount_ * 14 && !job_.counts[job_.layer])
    {
        ++job_.layer;
    }
    if (job_.layer == atlasCount_ * 14)
    {
        auto *target = job_.target;
        release(*target);
        *target = job_.result;
        job_.result = {};
        job_.active = false;
        job_.target = nullptr;
        return;
    }
    auto &result = job_.result;
    if (!result.buffer)
    {
        const std::size_t required = (job_.faceCount * 4 * sizeof(Vertex) + 63) & ~std::size_t(63);
        float targetDistance = 0;
        for (std::size_t i = 0; i < candidateCount_; ++i)
        {
            if (candidates_[i].pos == result.pos)
            {
                targetDistance = candidates_[i].distance;
                break;
            }
        }
        // Visibility never determines residency. Keep the nearest completed
        // meshes on both sides of the player, including when memory is full.
        std::size_t available = MeshBudget - meshBytes_;
        for (std::size_t i = 0; i < candidateCount_; ++i)
        {
            const auto &c = candidates_[i];
            if (c.mesh && c.mesh != job_.target && c.distance > targetDistance + 256)
            {
                available += c.mesh->bytes;
            }
        }
        if (required > available)
        {
            cancelPspBuild(true);
            return;
        }
        for (std::size_t i = candidateCount_; i > 0 && meshBytes_ + required > MeshBudget; --i)
        {
            auto &c = candidates_[i - 1];
            if (c.mesh && c.mesh != job_.target && c.distance > targetDistance + 256)
            {
                release(*c.mesh);
                c.mesh = nullptr;
            }
        }
        // PSPGL keeps one pending GL error. Do not attribute an earlier
        // UI/renderer error to this allocation.
        glGetError();
        glGenBuffersARB(1, &result.buffer);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, result.buffer);
        glBufferDataARB(GL_ARRAY_BUFFER_ARB, required, nullptr, GL_DYNAMIC_DRAW_ARB);
        const bool allocated = result.buffer && glGetError() == GL_NO_ERROR;
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        if (!allocated)
        {
            cancelPspBuild(true);
            return;
        }
        result.bytes = required;
        meshBytes_ += required;
    }
    if (job_.cursor == End)
    {
        job_.cursor = job_.heads[job_.layer];
    }
    const auto layer = static_cast<std::uint8_t>(job_.layer / 7);
    const auto direction = static_cast<std::uint8_t>(job_.layer % 7);
    if (!result.meshCount || result.batches[result.meshCount - 1].layer != layer ||
        result.batches[result.meshCount - 1].direction != direction ||
        result.batches[result.meshCount - 1].faces == FacesPerBatch)
    {
        if (result.meshCount == result.batches.size())
        {
            cancelPspBuild(true);
            return;
        }
        auto &batch = result.batches[result.meshCount++];
        batch = {job_.writtenFaces, 0, layer, direction};
    }
    auto &batch = result.batches[result.meshCount - 1];
    const unsigned count = std::min<unsigned>({FacesPerUpload, job_.counts[job_.layer], FacesPerBatch - batch.faces});
    // PSPGL unmaps a buffer when glBindBufferARB changes the binding. Do not
    // retain a mapped pointer across frames. Generate in cached CPU memory,
    // then copy only this slice into the unfinished section's VBO.
    alignas(16) std::array<Vertex, FacesPerUpload * 4> vertices;
    const auto started = sceKernelGetSystemTimeLow();
    unsigned written = 0;
    for (; written < count; ++written)
    {
        if (written && (written & 31) == 0 && sceKernelGetSystemTimeLow() - started >= 1000)
        {
            break;
        }
        const auto &placed = job_.faces[job_.cursor];
        const auto &face = faces_[placed.model];
        const unsigned tile = face.tile % 1024;
        // Q13 UVs retain the old 1/8192 inset and 1/16-pixel precision.
        const unsigned u = (tile % 32) * 256 + 1;
        const unsigned v = (tile / 32) * 256 + 1;
        const std::uint32_t color =
            face.color[0] | (std::uint32_t(face.color[1]) << 8) | (std::uint32_t(face.color[2]) << 16) | 0xff000000u;
        const float scaleY = placed.height * (1.0f / (255 * 256));
        for (unsigned k = 0; k < 4; ++k)
        {
            auto &vertex = vertices[written * 4 + k];
            vertex.u = static_cast<std::int16_t>(u + (face.uv[k * 2] * 254u + 127) / 255);
            vertex.v = static_cast<std::int16_t>(v + (face.uv[k * 2 + 1] * 254u + 127) / 255);
            vertex.color = color;
            vertex.x = placed.x + face.position[k * 3] * (1.0f / 256);
            vertex.y = placed.y + face.position[k * 3 + 1] * scaleY;
            vertex.z = placed.z + face.position[k * 3 + 2] * (1.0f / 256);
        }
        job_.cursor = placed.next;
    }
    const std::size_t offset = job_.writtenFaces * 4 * sizeof(Vertex);
    const std::size_t bytes = written * 4 * sizeof(Vertex);
    if (offset > result.bytes || bytes > result.bytes - offset)
    {
        cancelPspBuild(true);
        return;
    }
    glGetError();
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, result.buffer);
    glBufferSubDataARB(GL_ARRAY_BUFFER_ARB, offset, bytes, vertices.data());
    const bool uploaded = glGetError() == GL_NO_ERROR;
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
    if (!uploaded)
    {
        cancelPspBuild(true);
        return;
    }
    batch.faces += written;
    job_.writtenFaces += written;
    job_.counts[job_.layer] -= written;
}
void WorldRenderer::drawPsp(const World &world, Vector3 camera, Vector3 forward, float fovY, float viewDistance)
{
    if (!loaded_)
    {
        return;
    }
    const ViewVolume volume(forward, fovY);
    const float cacheDistance = viewDistance + 24;
    const SectionPos cell{static_cast<int>(std::floor(camera.x / 4)), static_cast<int>(std::floor(camera.y / 4)),
                          static_cast<int>(std::floor(camera.z / 4))};
    if (candidateRevision_ != world.revision() || !(candidateCell_ == cell))
    {
        candidateCell_ = cell;
        candidateRevision_ = world.revision();
        candidateCount_ = 0;
        world.forEach(
            [&](SectionPos pos, BlockState uniform, const World::Blocks *blocks, std::uint64_t version)
            {
                const float dx = pos.x * 16.0f + 8 - camera.x;
                const float dy = pos.y * 16.0f + 8 - camera.y;
                const float dz = pos.z * 16.0f + 8 - camera.z;
                const float distance = sectionDistanceSquared(dx, dy, dz) +
                                       0.001f * (dx * dx + dy * dy + dz * dz);
                if (distance >= cacheDistance * cacheDistance)
                {
                    return;
                }
                if (candidateCount_ < candidates_.size() && (blocks || !hiddenUniformSection(world, pos, uniform)))
                {
                    candidates_[candidateCount_++] = {pos, version, distance, nullptr, false};
                }
            });
        // The resident set is independent of camera direction. Turning must not
        // delete the ready meshes that will be needed when looking back.
        const auto count = std::min(candidateCount_, meshes_.size());
        std::partial_sort(candidates_.begin(), candidates_.begin() + count, candidates_.begin() + candidateCount_,
                          [](const Candidate &a, const Candidate &b)
                          {
                              return a.distance < b.distance;
                          });
        candidateCount_ = count;
        if (job_.active)
        {
            bool wanted = false;
            for (std::size_t i = 0; i < candidateCount_; ++i)
            {
                wanted |= candidates_[i].pos == job_.result.pos;
            }
            if (!wanted)
            {
                cancelPspBuild(false);
            }
        }
        for (auto &mesh : meshes_)
        {
            bool wanted = false;
            for (std::size_t i = 0; mesh.used && i < candidateCount_; ++i)
            {
                wanted |= mesh.pos == candidates_[i].pos;
            }
            if (!wanted)
            {
                release(mesh);
            }
        }
        // Cache pointers remain valid until a slot is explicitly released or
        // reused; no quadratic slot search is necessary on every rendered frame.
        for (std::size_t i = 0; i < candidateCount_; ++i)
        {
            auto &c = candidates_[i];
            for (auto &mesh : meshes_)
            {
                if (mesh.used && mesh.pos == c.pos)
                {
                    c.mesh = &mesh;
                    break;
                }
            }
        }
    }
    for (std::size_t i = 0; i < candidateCount_; ++i)
    {
        auto &c = candidates_[i];
        const float x = c.pos.x * 16.0f + 8 - camera.x;
        const float y = c.pos.y * 16.0f + 8 - camera.y;
        const float z = c.pos.z * 16.0f + 8 - camera.z;
        c.nearby = sectionDistanceSquared(x, y, z) <= viewDistance * viewDistance;
        c.visible = c.nearby && volume.contains(x, y, z);
    }
    if (!job_.active)
    {
        // Fill visible gaps before updating existing meshes or warming the rear ring.
        const double now = GetTime();
        for (std::size_t scan = 0; scan < candidateCount_ * 3; ++scan)
        {
            const std::size_t i = scan % candidateCount_;
            auto &c = candidates_[i];
            const bool foreground = scan < candidateCount_ * 2;
            const bool missing = !c.mesh || !c.mesh->version;
            if (!c.nearby || c.visible != foreground || (c.mesh && c.mesh->version == c.version) ||
                (foreground && missing != (scan < candidateCount_)))
            {
                continue;
            }
            auto *target = c.mesh;
            if (target && target->blocked && target->failedVersion == c.version && target->failureEpoch == budgetEpoch_ &&
                now < target->retryAfter)
            {
                continue;
            }
            if (!target)
            {
                for (auto &mesh : meshes_)
                {
                    if (!mesh.used)
                    {
                        target = &mesh;
                        break;
                    }
                }
            }
            if (!target)
            {
                // Spatial priority is the same when looking forwards or back.
                Candidate *victim = nullptr;
                for (std::size_t j = candidateCount_; j > 0; --j)
                {
                    auto &other = candidates_[j - 1];
                    if (other.mesh && other.distance > c.distance + 256)
                    {
                        victim = &other;
                        break;
                    }
                }
                if (victim)
                {
                    target = victim->mesh;
                    victim->mesh = nullptr;
                    release(*target);
                }
            }
            if (target)
            {
                beginPspBuild(*target, world, c.pos, c.version);
                c.mesh = target;
                break;
            }
        }
    }
    // All rebuilding precedes submissions: no writes to buffers used by this frame.
    advancePspBuild();
    const GLfloat fog[4] = {0.58f, 0.74f, 0.82f, 1};
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, viewDistance * (2.0f / 3));
    glFogf(GL_FOG_END, viewDistance);
    glFogfv(GL_FOG_COLOR, fog);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.12f);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, indexBuffer_);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glScalef(1.0f / 8192, 1.0f / 8192, 1);
    glMatrixMode(GL_MODELVIEW);
    glColor4ub(255, 255, 255, 255);
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 0)
        {
            glDisable(GL_BLEND);
        }
        else
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
        }
        unsigned texture = 0;
        // Opaque geometry needs only one texture bind per atlas. Translucent
        // sections keep their back-to-front order instead of sorting by atlas.
        for (unsigned atlasGroup = 0; atlasGroup < (pass == 0 ? atlasCount_ : 1); ++atlasGroup)
        {
            for (std::size_t j = 0; j < candidateCount_; ++j)
            {
                const auto &c = candidates_[pass == 0 ? j : candidateCount_ - 1 - j];
                if (!c.visible || !c.mesh || !c.mesh->meshCount)
                {
                    continue;
                }
                const float ox = c.pos.x * 16.0f;
                const float oy = c.pos.y * 16.0f;
                const float oz = c.pos.z * 16.0f;
                const bool facing[7] = {(camera.z < oz + 17),
                                        (camera.z > oz - 1),
                                        (camera.x < ox + 17),
                                        (camera.x > ox - 1),
                                        (camera.y > oy - 1),
                                        (camera.y < oy + 17),
                                        true};
                bool bound = false;
                for (std::size_t part = 0; part < c.mesh->meshCount; ++part)
                {
                    const auto &batch = c.mesh->batches[part];
                    if (batch.layer % 2 != pass ||
                        (pass == 0 && (batch.layer / 2 != atlasGroup || !facing[batch.direction])))
                    {
                        continue;
                    }
                    if (!bound)
                    {
                        glPushMatrix();
                        glTranslatef(ox, oy, oz);
                        glBindBufferARB(GL_ARRAY_BUFFER_ARB, c.mesh->buffer);
                        bound = true;
                    }
                    const auto atlas = materials_[batch.layer / 2].maps[MATERIAL_MAP_DIFFUSE].texture.id;
                    if (atlas != texture)
                    {
                        texture = atlas;
                        glBindTexture(GL_TEXTURE_2D, texture);
                    }
                    const std::size_t offset = batch.first * 4 * sizeof(Vertex);
                    glTexCoordPointer(2, GL_SHORT, sizeof(Vertex), reinterpret_cast<const void *>(offset));
                    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<const void *>(offset + 4));
                    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<const void *>(offset + 8));
                    glDrawRangeElements(GL_TRIANGLES, 0, batch.faces * 4 - 1, batch.faces * 6, GL_UNSIGNED_SHORT,
                                        nullptr);
                }
                if (bound)
                {
                    glPopMatrix();
                }
            }
        }
    }
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
    glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    // Release array-held references too; Raylib supplies its own pointers afterwards.
    glTexCoordPointer(2, GL_FLOAT, 0, nullptr);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
    glVertexPointer(3, GL_FLOAT, 0, nullptr);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
}
} // namespace craft
#endif
