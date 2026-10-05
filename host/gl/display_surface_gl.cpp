// Copyright (C) 2022 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "display_surface_gl.h"

#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <vector>

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#define MAS_RETURN_ADDRESS() _ReturnAddress()
#else
#define MAS_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include "OpenGLESDispatch/DispatchTables.h"
#include "OpenGLESDispatch/EGLDispatch.h"
#include "gfxstream/common/logging.h"
#include "gfxstream/threads/Thread.h"

namespace gfxstream {
namespace host {
namespace gl {
namespace {

struct PreviousContextInfo {
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface readSurface = EGL_NO_SURFACE;
    EGLSurface drawSurface = EGL_NO_SURFACE;
};

struct ThreadState {
    std::vector<PreviousContextInfo> previousContexts;
};

static thread_local ThreadState sThreadState;

class DisplaySurfaceGlContextHelper : public ContextHelper {
  public:
    DisplaySurfaceGlContextHelper(EGLDisplay display,
                                  EGLSurface surface,
                                  EGLContext context)
        : mDisplay(display),
          mSurface(surface),
          mContext(context) {
        if (mDisplay == EGL_NO_DISPLAY) {
            GFXSTREAM_FATAL("DisplaySurfaceGlContextHelper created with no display?");
        }
        if (mSurface == EGL_NO_SURFACE) {
            GFXSTREAM_FATAL("DisplaySurfaceGlContextHelper created with no surface?");
        }
        if (mContext == EGL_NO_CONTEXT) {
            GFXSTREAM_FATAL("DisplaySurfaceGlContextHelper created with no context?");
        }
    }

    bool setupContext() override {
        auto& previousContexts = sThreadState.previousContexts;

        EGLContext currentContext = s_egl.eglGetCurrentContext();
        EGLSurface currentDrawSurface = s_egl.eglGetCurrentSurface(EGL_DRAW);
        EGLSurface currentReadSurface = s_egl.eglGetCurrentSurface(EGL_READ);

        bool needsUpdate = (currentContext != mContext ||
                            currentDrawSurface != mSurface ||
                            currentReadSurface != mSurface);

        bool bound = true;
        if (needsUpdate) {
            if (!previousContexts.empty()) {
                GFXSTREAM_ERROR(
                    "DisplaySurfaceGlContextHelper context was preempted by others, "
                    "current=%p, needed=%p, thread=%p",
                    currentContext, mContext, &sThreadState);
                // Fall through to attempt to recover from error.
            }

            // MAS local: the WGL backend can report a failed bind as success, so check the
            // owner before binding instead of relying on the result.
            const void* const site = MAS_RETURN_ADDRESS();
            const unsigned long self = gfxstream::base::getCurrentThreadId();
            const unsigned long owner = mOwnerThread.load();
            if (owner != 0 && owner != self) {
                const SiteName mine = describeSite(site);
                const SiteName theirs = describeSite(mOwnerSite.load());
                GFXSTREAM_ERROR(
                    "MAS: thread %lu (site %s+0x%llx) binds context %p while thread %lu "
                    "(site %s+0x%llx) still has it bound",
                    self, mine.module, mine.offset, mContext, owner, theirs.module,
                    theirs.offset);
            }

            if (!s_egl.eglMakeCurrent(mDisplay, mSurface, mSurface, mContext)) {
                // b/284523053
                // Legacy swiftshader logspam on exit with this line.
                GFXSTREAM_DEBUG("Failed to make display surface context current: %d",
                                s_egl.eglGetError());
                // MAS local: say which thread and call site hold the context.
                const SiteName mine = describeSite(site);
                const SiteName theirs = describeSite(mOwnerSite.load());
                GFXSTREAM_ERROR(
                    "MAS: thread %lu (site %s+0x%llx) could not make context %p current; "
                    "thread %lu holds it (site %s+0x%llx)",
                    self, mine.module, mine.offset, mContext, mOwnerThread.load(),
                    theirs.module, theirs.offset);
                // Still add the previous context to the stack, so that
                // teardownContext() restores it, but report the failure:
                // GL calls made now would not reach this context.
                bound = false;
            } else {
                mOwnerThread.store(self);
                mOwnerSite.store(site);
            }
        }

        previousContexts.push_back(
            {.context = currentContext,
             .readSurface = currentReadSurface,
             .drawSurface = currentDrawSurface});
        return bound;
    }

    void teardownContext() override {
        auto& previousContexts = sThreadState.previousContexts;

        EGLContext currentContext = s_egl.eglGetCurrentContext();
        EGLSurface currentDrawSurface = s_egl.eglGetCurrentSurface(EGL_DRAW);
        EGLSurface currentReadSurface = s_egl.eglGetCurrentSurface(EGL_READ);

        PreviousContextInfo newContext;
        if (!previousContexts.empty()) {
            newContext = previousContexts.back();
            previousContexts.pop_back();
        }

        bool needsUpdate = (currentContext != newContext.context ||
                            currentDrawSurface != newContext.drawSurface ||
                            currentReadSurface != newContext.readSurface);

        if (!needsUpdate) {
            return;
        }

        if (newContext.context != mContext &&
            mOwnerThread.load() == gfxstream::base::getCurrentThreadId()) {
            mOwnerThread.store(0);
            mOwnerSite.store(nullptr);
        }

        if (!s_egl.eglMakeCurrent(mDisplay,
                                  newContext.drawSurface,
                                  newContext.readSurface,
                                  newContext.context)) {
            GFXSTREAM_ERROR("Failed to restore previous context: %d", s_egl.eglGetError());
        }
    }

    bool isBound() const override { return !sThreadState.previousContexts.empty(); }

  private:
    // MAS local: a call site as module file name + offset. Returned by value, so two
    // sites in one log line do not share a buffer. Without module lookup (non-Windows)
    // the module is "?" and the offset is the absolute address.
    struct SiteName {
        char module[64] = "?";
        unsigned long long offset = 0;
    };

    static SiteName describeSite(const void* site) {
        SiteName out;
        out.offset = reinterpret_cast<uintptr_t>(site);
#ifdef _WIN32
        HMODULE module = nullptr;
        if (site && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCSTR>(site), &module)) {
            out.offset = reinterpret_cast<uintptr_t>(site) - reinterpret_cast<uintptr_t>(module);
            char path[MAX_PATH];
            DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                const char* base = path;
                for (DWORD i = 0; i < n; ++i) {
                    if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
                }
                snprintf(out.module, sizeof(out.module), "%s", base);
            }
        }
#endif
        return out;
    }

    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLSurface mSurface = EGL_NO_SURFACE;
    EGLContext mContext = EGL_NO_CONTEXT;
    // MAS local: last thread / call site that made mContext current through this helper.
    std::atomic<unsigned long> mOwnerThread{0};
    std::atomic<const void*> mOwnerSite{nullptr};
};

}  // namespace

/*static*/
std::unique_ptr<DisplaySurfaceGl> DisplaySurfaceGl::createPbufferSurface(
        EGLDisplay display,
        EGLConfig config,
        EGLContext shareContext,
        const EGLint* contextAttribs,
        EGLint width,
        EGLint height) {
    EGLContext context = s_egl.eglCreateContext(display, config, shareContext, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        GFXSTREAM_ERROR("Failed to create context for DisplaySurfaceGl.");
        return nullptr;
    }

    const EGLint surfaceAttribs[] = {
        EGL_WIDTH, width,   //
        EGL_HEIGHT, height, //
        EGL_NONE,           //
    };
    EGLSurface surface = s_egl.eglCreatePbufferSurface(display, config, surfaceAttribs);
    if (surface == EGL_NO_SURFACE) {
        GFXSTREAM_ERROR("Failed to create pbuffer surface for DisplaySurfaceGl.");
        return nullptr;
    }

    return std::unique_ptr<DisplaySurfaceGl>(new DisplaySurfaceGl(display, surface, context));
}

/*static*/
std::unique_ptr<DisplaySurfaceGl> DisplaySurfaceGl::createWindowSurface(
        EGLDisplay display,
        EGLConfig config,
        EGLContext shareContext,
        const GLint* contextAttribs,
        FBNativeWindowType window) {
    EGLContext context = s_egl.eglCreateContext(display, config, shareContext, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        GFXSTREAM_ERROR("Failed to create context for DisplaySurfaceGl.");
        return nullptr;
    }

    EGLSurface surface = s_egl.eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        GFXSTREAM_ERROR("Failed to create window surface for DisplaySurfaceGl.");
        return nullptr;
    }

    return std::unique_ptr<DisplaySurfaceGl>(new DisplaySurfaceGl(display, surface, context));
}

bool DisplaySurfaceGl::bindContext() const {
    if (!s_egl.eglMakeCurrent(mDisplay, mSurface, mSurface, mContext)) {
        GFXSTREAM_ERROR("Failed to make display surface context current: %d", s_egl.eglGetError());
        return false;
    }
    return true;
}

DisplaySurfaceGl::DisplaySurfaceGl(EGLDisplay display,
                                   EGLSurface surface,
                                   EGLContext context)
    : mDisplay(display),
      mSurface(surface),
      mContext(context),
      mContextHelper(new DisplaySurfaceGlContextHelper(display, surface, context)) {}

DisplaySurfaceGl::~DisplaySurfaceGl() {
    if (mDisplay != EGL_NO_DISPLAY) {
        if (mSurface) {
            s_egl.eglDestroySurface(mDisplay, mSurface);
        }
        if (mContext) {
            s_egl.eglDestroyContext(mDisplay, mContext);
        }
    }
}

}  // namespace gl
}  // namespace host
}  // namespace gfxstream