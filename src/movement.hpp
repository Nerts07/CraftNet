#pragma once
#include "session.hpp"
#include "render.hpp"

namespace craft
{
struct MovementInput
{
    float forward = 0, side = 0;
    bool jump = false, jumpHeld = false, sprint = false, sneak = false;
};

struct ViewBob
{
    float side, up, roll, pitch;
};

class Movement final
{
  public:
    // Teleport corrections snap interpolation, but must not restart the tick
    // clock: frequent corrections could otherwise prevent all 50 ms steps.
    void reset(bool preserveClock = false)
    {
        vx_ = vy_ = vz_ = 0;
        if (!preserveClock)
        {
            accumulator_ = 0;
            eyeHeight_ = 1.62f;
            crouched_ = false;
            walkPhase_ = previousWalkPhase_ = bobAmount_ = previousBobAmount_ = 0;
        }
        jumpQueued_ = false;
        initialized_ = false;
    }
    // Advances movement using a fixed 20 Hz simulation tick.
    void update(PlayerPose &pose, const World &world, const WorldRenderer &render, const MovementInput &input, float dt,
                Session *session = nullptr);
    // Returns the interpolated pose used for smooth rendering.
    PlayerPose visual(const PlayerPose &pose) const;
    float eyeHeight() const { return eyeHeight_; }
    ViewBob viewBob() const;
    // Checks whether the player collision box overlaps a solid block.
    static bool collides(const World &world, const WorldRenderer &render, double x, double y, double z,
                         float height = 1.8f);

  private:
    // Runs one fixed simulation step.
    void tick(PlayerPose &pose, const World &world, const WorldRenderer &render, const MovementInput &input);
    float vx_ = 0, vy_ = 0, vz_ = 0;
    float accumulator_ = 0;
    float eyeHeight_ = 1.62f;
    float walkPhase_ = 0, previousWalkPhase_ = 0;
    float bobAmount_ = 0, previousBobAmount_ = 0;
    bool crouched_ = false;
    bool jumpQueued_ = false;
    bool initialized_ = false;
    PlayerPose previous_{};
};
} // namespace craft
