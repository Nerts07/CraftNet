#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>

namespace craft
{
inline Camera2D uiCamera()
{
    Camera2D camera{};
    camera.zoom = 1;
#if defined(_WIN32)
    const float width = static_cast<float>(GetScreenWidth());
    const float height = static_cast<float>(GetScreenHeight());
    camera.zoom = std::min(width / 480, height / 272);
    camera.offset = {(width - 480 * camera.zoom) / 2, (height - 272 * camera.zoom) / 2};
#endif
    return camera;
}

inline void beginUiScissor(float x, float y, float width, float height)
{
    const auto camera = uiCamera();
    const float left = camera.offset.x + x * camera.zoom;
    const float top = camera.offset.y + y * camera.zoom;
    const int startX = static_cast<int>(std::floor(left));
    const int startY = static_cast<int>(std::floor(top));
    BeginScissorMode(startX, startY, static_cast<int>(std::ceil(left + width * camera.zoom)) - startX,
                     static_cast<int>(std::ceil(top + height * camera.zoom)) - startY);
}
} // namespace craft
