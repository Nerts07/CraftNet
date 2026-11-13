#pragma once

#if defined(__PSP__)
#include <raylib.h>
#include <rlgl.h>
#include <GL/gl.h>
#include <pspge.h>

namespace craft
{
// Utility GU lists share the framebuffer and GE state with PSPGL. Submit a
// complete, unclipped background before letting a utility render its own UI.
inline int drawPspDialog(int (*update)(int))
{
    EndScissorMode();
    glViewport(0, 0, 480, 272);
    glScissor(0, 0, 480, 272);
    DrawRectangle(0, 0, 480, 272, Color{20, 29, 33, 255});
    rlDrawRenderBatchActive();
    glFinish();
    alignas(16) PspGeContext context{};
    const bool saved = sceGeSaveContext(&context) >= 0;
    const int result = update(1);
    sceGeDrawSync(0);
    if (saved)
    {
        sceGeRestoreContext(&context);
    }
    return result;
}
} // namespace craft
#endif
