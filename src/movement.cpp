#include "movement.hpp"
#include "blocks.hpp"
#include <algorithm>
#include <cmath>

namespace craft
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
}

// Checks whether the player collision box overlaps a solid block.
bool Movement::collides(const World &world, const WorldRenderer &render, double x, double y, double z, float height)
{
    // Keep global coordinates in double, but collision arithmetic local to a block
    // uses PSP's native single-precision FPU rather than software double routines.
    const int centerX = static_cast<int>(std::floor(x));
    const int centerY = static_cast<int>(std::floor(y));
    const int centerZ = static_cast<int>(std::floor(z));
    const float fx = static_cast<float>(x - centerX);
    const float fy = static_cast<float>(y - centerY);
    const float fz = static_cast<float>(z - centerZ);
    const int minX = centerX + (fx < 0.299999f ? -1 : 0);
    const int maxX = centerX + (fx + 0.299999f >= 1 ? 1 : 0);
    const int feetY = centerY;
    const int minY = centerY + (fy < 0.499999f ? -1 : 0);
    const int maxY = centerY + static_cast<int>(fy + height - 0.000001f);
    const int minZ = centerZ + (fz < 0.299999f ? -1 : 0);
    const int maxZ = centerZ + (fz + 0.299999f >= 1 ? 1 : 0);
    for (int by = minY; by <= maxY; ++by)
    {
        for (int bz = minZ; bz <= maxZ; ++bz)
        {
            for (int bx = minX; bx <= maxX; ++bx)
            {
                BlockState state;
                if (!world.get(bx, by, bz, state))
                {
                    if (by >= feetY)
                    {
                        return true;
                    }
                    continue;
                }
                const float lx = static_cast<float>(centerX - bx) + fx;
                const float ly = static_cast<float>(centerY - by) + fy;
                const float lz = static_cast<float>(centerZ - bz) + fz;
                if (render.intersects(state, lx - 0.299999f, ly + 0.000001f, lz - 0.299999f, lx + 0.299999f,
                                      ly + height - 0.000001f, lz + 0.299999f))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// Advances movement using a fixed 20 Hz simulation tick.
void Movement::update(PlayerPose &p, const World &world, const WorldRenderer &render, const MovementInput &input,
                      float dt, Session *session)
{
    // The session can replace velocity without teleporting (knockback, pads).
    // Keep the fixed-step clock when consuming such an update.
    vx_ = p.vx;
    vy_ = p.vy;
    vz_ = p.vz;
    if (!initialized_)
    {
        previous_ = p;
        previousWalkPhase_ = walkPhase_;
        previousBobAmount_ = bobAmount_;
        initialized_ = true;
    }
    jumpQueued_ |= input.jump;
    crouched_ = input.sneak || (crouched_ && collides(world, render, p.x, p.y, p.z));
    const float eyeTarget = crouched_ ? 1.27f : 1.62f;
    eyeHeight_ += (eyeTarget - eyeHeight_) * (1 - std::exp2(-std::clamp(dt, 0.0f, 0.1f) / 0.05f));
    p.input = (input.forward > 0.1f ? 1 : 0) | (input.forward < -0.1f ? 2 : 0) | (input.side < -0.1f ? 4 : 0) |
              (input.side > 0.1f ? 8 : 0) | (input.jumpHeld || input.jump ? 16 : 0) | (input.sneak ? 32 : 0) |
              (input.sprint && input.forward > 0.1f && !crouched_ ? 64 : 0);
    const auto controls = p.input;
    // After a stalled frame, limit catch-up to two ticks so the server
    // does not receive a burst of movement for the entire stall.
    accumulator_ += std::clamp(dt, 0.0f, 0.1f);
    for (int i = 0; i < 2 && accumulator_ >= 0.05f; ++i)
    {
        previous_ = p;
        previousWalkPhase_ = walkPhase_;
        previousBobAmount_ = bobAmount_;
        p.input = controls;
        if (session && session->spectator())
        {
            const float angle = p.yaw * Pi / 180.0f;
            const float forward = std::clamp(input.forward, -1.0f, 1.0f);
            const float side = std::clamp(input.side, -1.0f, 1.0f);
            const float speed = 0.30f / std::max(1.0f, std::sqrt(forward * forward + side * side));
            p.vx = p.vy = p.vz = 0;
            p.onGround = p.horizontalCollision = false;
            if (!session->cameraTarget())
            {
                p.x += (-std::sin(angle) * forward - std::cos(angle) * side) * speed;
                p.z += (std::cos(angle) * forward - std::sin(angle) * side) * speed;
                p.y += (static_cast<int>(input.jumpHeld) - static_cast<int>(input.sneak)) * 0.30f;
            }
        }
        else
        {
            tick(p, world, render, input);
        }
        const float dx = static_cast<float>(p.x - previous_.x), dz = static_cast<float>(p.z - previous_.z);
        const float travel = std::sqrt(dx * dx + dz * dz);
        const float stride = p.onGround ? std::min(travel, 0.36f) : 0.0f;
        bobAmount_ += (stride - bobAmount_) * 0.24f;
        walkPhase_ += bobAmount_ * 2;
        if (walkPhase_ > 2 * Pi)
        {
            walkPhase_ -= 2 * Pi;
            previousWalkPhase_ -= 2 * Pi;
        }
        if (session)
        {
            session->movementTick(p);
        }
        accumulator_ -= 0.05f;
    }
}

ViewBob Movement::viewBob() const
{
    const float alpha = std::clamp(accumulator_ / 0.05f, 0.0f, 1.0f);
    const float phase = previousWalkPhase_ + (walkPhase_ - previousWalkPhase_) * alpha;
    const float amount = previousBobAmount_ + (bobAmount_ - previousBobAmount_) * alpha;
    const float swing = std::sin(phase) * amount;
    const float rise = (0.5f - 0.5f * std::cos(2 * phase)) * amount;
    return {swing * 0.09f, -rise * 0.055f, swing * 0.9f, std::cos(phase + 0.35f) * amount * 0.9f};
}

// Returns the interpolated pose used for smooth rendering.
PlayerPose Movement::visual(const PlayerPose &p) const
{
    if (!initialized_)
    {
        return p;
    }
    const double alpha = std::max(0.0, std::min(1.0, static_cast<double>(accumulator_) / 0.05));
    PlayerPose out = p;
    out.x = previous_.x + (p.x - previous_.x) * alpha;
    out.y = previous_.y + (p.y - previous_.y) * alpha;
    out.z = previous_.z + (p.z - previous_.z) * alpha;
    return out;
}

// Runs one fixed simulation step.
void Movement::tick(PlayerPose &p, const World &world, const WorldRenderer &render, const MovementInput &input)
{
    const bool grounded = p.onGround;
    const bool sprint = (p.input & 64) != 0;
    const float height = crouched_ ? 1.5f : 1.8f;
    bool inWater = false;
    const int waterY = static_cast<int>(std::floor(p.y + 0.01f));
    for (int y = waterY; y <= static_cast<int>(std::floor(p.y + height - 0.01f)); ++y)
    {
        for (int z = static_cast<int>(std::floor(p.z - 0.29f)); z <= static_cast<int>(std::floor(p.z + 0.29f)); ++z)
        {
            for (int x = static_cast<int>(std::floor(p.x - 0.29f)); x <= static_cast<int>(std::floor(p.x + 0.29f)); ++x)
            {
                const float height = render.waterHeight(world, x, y, z);
                inWater |= height > 0 && p.y + 0.01f < y + height;
            }
        }
    }
    if (inWater && input.jumpHeld)
    {
        vy_ = std::min(0.18f, vy_ + 0.04f);
    }
    else if ((jumpQueued_ || input.jumpHeld) && p.onGround)
    {
        vy_ = std::max(vy_, 0.42f);
        p.input |= 16;
        p.onGround = false;
        if (sprint)
        {
            const float a = p.yaw * Pi / 180.0f;
            vx_ -= std::sin(a) * 0.2f;
            vz_ += std::cos(a) * 0.2f;
        }
    }
    jumpQueued_ = false;
    // Protocol 777 transmits digital input, not joystick magnitude. Simulate
    // exactly those same directions so prediction and transmitted input agree.
    float forward = (p.input & 1) ? 1.0f : (p.input & 2) ? -1.0f : 0.0f;
    float side = (p.input & 8) ? 1.0f : (p.input & 4) ? -1.0f : 0.0f;
    float len = std::sqrt(forward * forward + side * side);
    if (len > 1.0f)
    {
        forward /= len;
        side /= len;
    }
    forward *= 0.98f;
    side *= 0.98f;
    if (crouched_)
    {
        forward *= 0.3f;
        side *= 0.3f;
    }
    // Java's 20 Hz travel loop: ground/air acceleration, drag and gravity.
    const float friction = inWater ? 0.8f : (grounded ? 0.546f : 0.91f);
    const float acceleration = (inWater ? 0.02f : (grounded ? p.moveSpeed : 0.02f)) * (sprint ? 1.3f : 1.0f);
    const float angle = p.yaw * Pi / 180.0f;
    vx_ += (-std::sin(angle) * forward - std::cos(angle) * side) * acceleration;
    vz_ += (std::cos(angle) * forward - std::sin(angle) * side) * acceleration;
    auto axis = [&](int component, float requested)
    {
        if (!requested)
        {
            return false;
        }
        constexpr double radius = static_cast<double>(0.6f) * 0.5;
        const double low[3] = {p.x - radius, p.y, p.z - radius};
        const double high[3] = {p.x + radius, p.y + height, p.z + radius};
        double sweptLow[3] = {low[0], low[1], low[2]};
        double sweptHigh[3] = {high[0], high[1], high[2]};
        double delta = std::clamp(static_cast<double>(requested), -16.0, 16.0);
        sweptLow[component] += std::min(0.0, delta);
        sweptHigh[component] += std::max(0.0, delta);
        const int minX = static_cast<int>(std::floor(sweptLow[0]));
        const int minY = static_cast<int>(std::floor(sweptLow[1])) - 1;
        const int minZ = static_cast<int>(std::floor(sweptLow[2]));
        const int maxX = static_cast<int>(std::floor(sweptHigh[0] - 1e-9));
        const int maxY = static_cast<int>(std::floor(sweptHigh[1] - 1e-9));
        const int maxZ = static_cast<int>(std::floor(sweptHigh[2] - 1e-9));
        for (int y = minY; y <= maxY && delta; ++y)
        {
            for (int z = minZ; z <= maxZ && delta; ++z)
            {
                for (int x = minX; x <= maxX && delta; ++x)
                {
                    BlockState state;
                    if (!world.get(x, y, z, state))
                    {
                        // Do not manufacture a solid floor in an unloaded lower
                        // section, or move into a missing section ahead.
                        if (y >= static_cast<int>(std::floor(p.y)))
                        {
                            delta = 0;
                        }
                        continue;
                    }
                    if (!render.isSolid(state))
                    {
                        continue;
                    }
                    const double localLow[3] = {low[0] - x, low[1] - y, low[2] - z};
                    const double localHigh[3] = {high[0] - x, high[1] - y, high[2] - z};
                    delta = render.clipAxis(state, component, localLow, localHigh, delta);
                }
            }
        }
        if (component == 0)
        {
            p.x += delta;
        }
        else if (component == 1)
        {
            p.y += delta;
        }
        else
        {
            p.z += delta;
        }
        return delta != static_cast<double>(requested);
    };
    const bool blockedY = axis(1, vy_);
    p.onGround = blockedY && vy_ < 0;
    if (blockedY)
    {
        vy_ = 0;
    }
    const PlayerPose base = p;
    float requestedX = vx_;
    float requestedZ = vz_;
    if (input.sneak && (grounded || p.onGround) && vy_ <= 0 && !inWater)
    {
        const auto supported = [&](float x, float z)
        {
            p.x = base.x + x;
            p.z = base.z + z;
            const bool floor = axis(1, -0.6f);
            p = base;
            return floor;
        };
        const auto trim = [](float value) { return std::copysign(std::max(0.0f, std::abs(value) - 0.05f), value); };
        requestedX = std::clamp(requestedX, -16.0f, 16.0f);
        requestedZ = std::clamp(requestedZ, -16.0f, 16.0f);
        while (requestedX && !supported(requestedX, 0))
        {
            requestedX = trim(requestedX);
        }
        while (requestedZ && !supported(0, requestedZ))
        {
            requestedZ = trim(requestedZ);
        }
        while (requestedX && requestedZ && !supported(requestedX, requestedZ))
        {
            requestedX = trim(requestedX);
            requestedZ = trim(requestedZ);
        }
        vx_ = requestedX;
        vz_ = requestedZ;
    }
    const auto horizontal = [&axis](float x, float z, bool &blockedX, bool &blockedZ)
    {
        if (std::abs(x) < std::abs(z))
        {
            blockedZ = axis(2, z);
            blockedX = axis(0, x);
        }
        else
        {
            blockedX = axis(0, x);
            blockedZ = axis(2, z);
        }
    };
    bool blockedX, blockedZ;
    horizontal(requestedX, requestedZ, blockedX, blockedZ);
    const bool walking = len > 0.01f && !crouched_ && !inWater;
    if ((blockedX || blockedZ) && p.onGround && walking)
    {
        const PlayerPose flat = p;
        const double flatDistance = (p.x - base.x) * (p.x - base.x) + (p.z - base.z) * (p.z - base.z);
        p = base;
        axis(1, 0.6f);
        const double rise = p.y - base.y;
        bool stepX, stepZ;
        horizontal(requestedX, requestedZ, stepX, stepZ);
        const bool landed = rise > 0.0001f && axis(1, -rise - 0.0001f);
        const double stepDistance = (p.x - base.x) * (p.x - base.x) + (p.z - base.z) * (p.z - base.z);
        if (landed && stepDistance > flatDistance + 0.000001f)
        {
            p.onGround = true;
            blockedX = stepX;
            blockedZ = stepZ;
        }
        else
        {
            p = flat;
            // Jump only toward a supported ledge, with a clear ascent and landing volume.
            const double aheadX = p.x + requestedX * 2;
            const double aheadZ = p.z + requestedZ * 2;
            if (!collides(world, render, p.x, p.y + 1.25f, p.z) &&
                !collides(world, render, aheadX, p.y + 1.001f, aheadZ) &&
                collides(world, render, aheadX, p.y + 0.6f, aheadZ))
            {
                // Apply the jump at the next tick's start, together with its
                // input bit and normal ground acceleration/drag.
                jumpQueued_ = true;
            }
        }
    }
    if (blockedX)
    {
        vx_ = 0;
    }
    p.horizontalCollision = blockedX || blockedZ;
    if (blockedZ)
    {
        vz_ = 0;
    }
    if (inWater && input.jumpHeld && (blockedX || blockedZ) && !collides(world, render, p.x, p.y + 0.6f, p.z, height))
    {
        vy_ = 0.3f;
    }
    vx_ *= friction;
    vz_ *= friction;
    vy_ = inWater ? (vy_ - 0.02f) * 0.8f : (vy_ - 0.08f) * 0.98f;
    if (std::abs(vx_) < 0.003f)
    {
        vx_ = 0;
    }
    if (std::abs(vz_) < 0.003f)
    {
        vz_ = 0;
    }
    if (std::abs(vy_) < 0.003f)
    {
        vy_ = 0;
    }
    p.vx = vx_;
    p.vy = vy_;
    p.vz = vz_;
}
} // namespace craft
