#pragma once
#include <raylib.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace craft
{
// Squared distance from the camera to the nearest point of a section.
inline float sectionDistanceSquared(float x, float y, float z)
{
    x = std::max(0.0f, std::abs(x) - 8);
    y = std::max(0.0f, std::abs(y) - 8);
    z = std::max(0.0f, std::abs(z) - 8);
    return x * x + y * y + z * z;
}

class ViewVolume
{
  public:
    ViewVolume(Vector3 forward, float fovY)
    {
        const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
        const Vector3 right = length > 0.0001f ? Vector3{forward.z / length, 0, -forward.x / length} : Vector3{1, 0, 0};
        const Vector3 up{forward.y * right.z, forward.z * right.x - forward.x * right.z, -forward.y * right.x};
        const float vertical = std::tan(fovY * (DEG2RAD / 2));
        const float horizontal = vertical * GetScreenWidth() / GetScreenHeight();
        planes_[0] = forward;
        for (int i = 0; i < 4; ++i)
        {
            const auto axis = i < 2 ? right : up;
            const float tangent = i < 2 ? horizontal : vertical;
            const float sign = (i & 1) ? -1.0f : 1.0f;
            planes_[i + 1] = {forward.x * tangent + axis.x * sign, forward.y * tangent + axis.y * sign,
                              forward.z * tangent + axis.z * sign};
        }
    }
    bool contains(float x, float y, float z, float extent = 9) const
    {
        for (const auto &normal : planes_)
        {
            const float support = extent * (std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z));
            if (normal.x * x + normal.y * y + normal.z * z < -support)
            {
                return false;
            }
        }
        return true;
    }

  private:
    std::array<Vector3, 5> planes_{};
};
} // namespace craft
