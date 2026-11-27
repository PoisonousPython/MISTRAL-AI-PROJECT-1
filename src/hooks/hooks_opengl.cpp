#include "tgov_win.h"

namespace tgov {

namespace {

typedef BOOL(WINAPI* PFN_wglSwapBuffers)(HDC);
typedef BOOL(WINAPI* PFN_wglSwapLayerBuffers)(HDC, UINT);
typedef unsigned int EGLBoolean;
typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef EGLBoolean(WINAPI* PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);

PFN_wglSwapBuffers RealWglSwapBuffers = nullptr;
PFN_wglSwapLayerBuffers RealWglSwapLayerBuffers = nullptr;
PFN_eglSwapBuffers RealEglSwapBuffers = nullptr;

BOOL WINAPI HookWglSwapBuffers(HDC dc) {
    PacerOpenGl().Wait();
    return RealWglSwapBuffers(dc);
}

BOOL WINAPI HookWglSwapLayerBuffers(HDC dc, UINT planes) {
    PacerOpenGl().Wait();
    return RealWglSwapLayerBuffers(dc, planes);
}

EGLBoolean WINAPI HookEglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    PacerOpenGl().Wait();
    return RealEglSwapBuffers(display, surface);
}

}  // namespace

unsigned InstallOpenGLHooks() {
    unsigned n = 0;
    if (!RealWglSwapBuffers) {
        n += HookIatAllModules("opengl32.dll", "wglSwapBuffers", reinterpret_cast<void*>(&HookWglSwapBuffers),
                               reinterpret_cast<void**>(&RealWglSwapBuffers));
    }
    if (!RealWglSwapLayerBuffers) {
        n += HookIatAllModules("opengl32.dll", "wglSwapLayerBuffers", reinterpret_cast<void*>(&HookWglSwapLayerBuffers),
                               reinterpret_cast<void**>(&RealWglSwapLayerBuffers));
    }
    const char* egl_modules[] = {"libegl.dll", "libegl_angle.dll"};
    for (const char* egl : egl_modules) {
        if (RealEglSwapBuffers) break;
        n += HookIatAllModules(egl, "eglSwapBuffers", reinterpret_cast<void*>(&HookEglSwapBuffers),
                               reinterpret_cast<void**>(&RealEglSwapBuffers));
    }
    if (n) Logf("opengl: swap hooks installed (%u sites)", n);
    return n;
}

}  // namespace tgov
