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

#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES3/gl3.h>

#include <array>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

namespace android_studio {
class EmulatorGLESUsages;
}

namespace gfxstream {
struct RenderOpt;
}

#include "OpenGLESDispatch/EGLDispatch.h"
#include "OpenGLESDispatch/GLESv2Dispatch.h"
#include "buffer_gl.h"
#include "color_buffer_gl.h"
#include "compositor_gl.h"
#include "context_helper.h"
#include "display_gl.h"
#include "emulated_egl_config.h"
#include "emulated_egl_context.h"
#include "emulated_egl_fence_sync.h"
#include "emulated_egl_image.h"
#include "emulated_egl_window_surface.h"
#include "gfxstream/host/color_buffer_interface.h"
#include "gfxstream/host/compositor.h"
#include "gfxstream/host/display.h"
#include "gfxstream/host/display_surface.h"
#include "gfxstream/host/display_surface_user.h"
#include "gfxstream/host/features.h"
#include "gfxstream/host/framework_formats.h"
#include "gfxstream/host/gfxstream_format.h"
#include "gfxstream/host/gl_enums.h"
#include "gfxstream/host/global_state.h"
#include "gfxstream/synchronization/Lock.h"
#include "pixel_read_formats.h"
#include "readback_worker_gl.h"
#include "render-utils/stream.h"
#include "texture_draw.h"

#define EGL_NO_CONFIG ((EGLConfig)0)

namespace gfxstream {
namespace host {
namespace gl {

class EmulationGl {
   public:
    static bool initDispatchers(bool eglOnEgl);
    static std::unique_ptr<EmulationGl> create(uint32_t width, uint32_t height,
                                               const FeatureSet& features, bool allowWindowSurface,
                                               GlobalState* globalState);

    ~EmulationGl();

    static const EGLDispatch* getEglDispatch();
    static const GLESv2Dispatch* getGles2Dispatch();

    std::string getEglString(EGLenum name);
    std::string getGlString(EGLenum name);

    GLESDispatchMaxVersion getGlesMaxDispatchVersion() const;

    static const GLint* getGlesMaxContextAttribs();

    bool hasEglExtension(const std::string& ext) const;
    void getEglVersion(EGLint* major, EGLint* minor) const;

    void getGlesVersion(GLint* major, GLint* minor) const;
    const std::string& getGlesVendor() const { return mGlesVendor; }
    const std::string& getGlesRenderer() const { return mGlesRenderer; }
    const std::string& getGlesVersionString() const { return mGlesVersion; }
    const std::string& getGlesExtensionsString() const { return mGlesExtensions; }
    bool isGlesVulkanInteropSupported() const { return mGlesVulkanInteropSupported; }

    bool isMesa() const;

    bool isFastBlitSupported() const;
    void disableFastBlitForTesting();

    bool isAsyncReadbackSupported() const;

    std::unique_ptr<DisplaySurface> createWindowSurface(uint32_t width,
                                                        uint32_t height,
                                                        EGLNativeWindowType window);

    const EmulatedEglConfigList& getEmulationEglConfigs() const { return *mEmulatedEglConfigs; }

    CompositorGl* getCompositor() { return mCompositorGl.get(); }

    DisplayGl* getDisplay() { return mDisplayGl.get(); }
    EGLDisplay getEglDisplay() const { return mEglDisplay; }
    DisplaySurface* getPbufferSurface() const { return mPbufferSurface.get(); }
    TextureDraw* getTextureDraw() const { return mTextureDraw.get(); }

    ReadbackWorkerGl* getReadbackWorker() { return mReadbackWorkerGl.get(); }

    using GlesUuid = std::array<uint8_t, GL_UUID_SIZE_EXT>;
    const std::optional<GlesUuid> getGlesDeviceUuid() const { return mGlesDeviceUuid; }

    std::unique_ptr<BufferGl> createBuffer(uint64_t size, HandleType handle);

    std::unique_ptr<BufferGl> loadBuffer(Stream* stream);

    bool isFormatSupported(GfxstreamFormat format);

    std::unique_ptr<ColorBufferGl> createColorBuffer(uint32_t width, uint32_t height,
                                                     GfxstreamFormat format,
                                                     HandleType handle);

    std::unique_ptr<ColorBufferGl> loadColorBuffer(Stream* stream);

    HandleType createEmulatedEglContext(uint32_t emulatedConfigIndex, HandleType shareContextHandle,
                                        GLESApi api);

    void destroyEmulatedEglContext(HandleType contextHandle);

    HandleType createEmulatedEglWindowSurface(uint32_t emulatedConfigIndex, uint32_t width,
                                              uint32_t height);

    std::vector<HandleType> destroyEmulatedEglWindowSurface(HandleType surfaceHandle);

    bool isHandleInUse(HandleType handle) const;

    EmulatedEglContextPtr getContext(HandleType contextHandle);
    EmulatedEglWindowSurfacePtr getWindowSurface(HandleType surfaceHandle);

    uint64_t createEmulatedEglFenceSync(EGLenum type, int destroyWhenSignaled);

    HandleType createEmulatedEglImage(HandleType contextHandle, EGLenum target,
                                      EGLClientBuffer buffer);

    bool destroyEmulatedEglImage(HandleType imageHandle);

    std::unique_ptr<DisplaySurface> createFakeWindowSurface();

    bool bindColorBufferToTexture(IColorBuffer* colorBuffer);
    bool bindColorBufferToTexture2(IColorBuffer* colorBuffer);
    bool bindColorBufferToRenderbuffer(IColorBuffer* colorBuffer);
    bool bindContext(HandleType contextHandle, HandleType drawSurfaceHandle,
                     HandleType readSurfaceHandle);
    bool bindColorBufferToWindowSurface(HandleType surfaceHandle, HandleType colorBufferHandle,
                                        HandleType* outOldColorBufferHandle);
    HandleType getWindowSurfaceColorBufferHandle(HandleType surfaceHandle) const;

    void preSave(Stream* stream, const gfxstream::ITextureSaverPtr& textureSaver);
    void postSave(Stream* stream);
    void saveContexts(Stream* stream);
    void saveWindowSurfaces(Stream* stream);
    void saveProcOwnedWindowSurfaces(Stream* stream);
    void saveProcOwnedContexts(Stream* stream);
    void saveProcOwnedImages(Stream* stream);

    bool loadContexts(Stream* stream);
    bool loadWindowSurfaces(Stream* stream,
                            const std::function<IColorBufferRef(uint32_t)>& colorBufferLookup);
    void loadProcOwnedWindowSurfaces(Stream* stream);
    void loadProcOwnedContexts(Stream* stream);
    void loadProcOwnedImages(Stream* stream);
    void loadAllImages(Stream* stream, const gfxstream::ITextureLoaderPtr& textureLoader);
    void postLoad(Stream* stream);
    std::vector<HandleType> cleanupProcGLObjects(uint64_t puid);
    bool hasContextsOrWindowSurfaces() const;
    void clearContextsAndWindowSurfaces();
    bool hasProcOwnedResources() const;
    std::vector<uint64_t> getGLPUIDs() const;
    // MAS local diagnostic: how many contexts, window surfaces and EGLImages exist.
    void getObjectCounts(size_t* contexts, size_t* windowSurfaces, size_t* images) const;
    void drainRenderThreadContexts();
    void drainRenderThreadSurfaces();
    void drainRenderThreadResources();
    bool flushEmulatedEglWindowSurfaceColorBuffer(HandleType surfaceHandle);
    void fillGlesUsages(android_studio::EmulatorGLESUsages* usages);
    bool getRenderOpt(gfxstream::RenderOpt* opt) const;
    ContextHelper* getPbufferSurfaceContextHelper() const;
    EGLContext getGlobalEGLContext() const;

    void lockContextStructureRead() { mContextStructureLock.lockRead(); }
    void unlockContextStructureRead() { mContextStructureLock.unlockRead(); }

    void createTrivialContext(HandleType shared, HandleType* contextOut, HandleType* surfOut);
    void createSharedTrivialContext(EGLContext* contextOut, EGLSurface* surfOut);
    void destroySharedTrivialContext(EGLContext context, EGLSurface surface);
    bool setEmulatedEglWindowSurfaceColorBuffer(HandleType surfaceHandle,
                                                HandleType colorBufferHandle);
    void* platformCreateSharedEglContext();
    bool platformDestroySharedEglContext(void* underlyingContext);

    void createYUVTextures(uint32_t type, uint32_t count, int width, int height, uint32_t* output);
    void destroyYUVTextures(uint32_t type, uint32_t count, uint32_t* textures);
    void updateYUVTextures(uint32_t type, uint32_t* textures, void* privData, void* func);
    void swapTexturesAndUpdateColorBuffer(IColorBuffer* colorBuffer, uint32_t format, uint32_t type,
                                          uint32_t texturesType, uint32_t* textures);

   private:
    EmulationGl() = default;

    std::unique_ptr<EmulatedEglContext> createEmulatedEglContextImpl(
        uint32_t emulatedEglConfigIndex, const EmulatedEglContext* shareContext, GLESApi api,
        HandleType handle);

    std::unique_ptr<EmulatedEglContext> loadEmulatedEglContext(Stream* stream);

    std::unique_ptr<EmulatedEglWindowSurface> createEmulatedEglWindowSurfaceImpl(
        uint32_t emulatedConfigIndex, uint32_t width, uint32_t height, HandleType handle);

    std::unique_ptr<EmulatedEglWindowSurface> loadEmulatedEglWindowSurface(
        Stream* stream, const std::function<IColorBufferRef(uint32_t)>& colorBufferLookup,
        const EmulatedEglContextMap& contexts);

    ContextHelper* getColorBufferContextHelper();

    FeatureSet mFeatures;

    EGLDisplay mEglDisplay = EGL_NO_DISPLAY;
    EGLint mEglVersionMajor = 0;
    EGLint mEglVersionMinor = 0;
    std::string mEglVendor;
    std::unordered_set<std::string> mEglExtensions;
    EGLConfig mEglConfig = EGL_NO_CONFIG;

    // The "global" context that all other contexts are shared with.
    EGLContext mEglContext = EGL_NO_CONTEXT;

    // Used for ColorBuffer ops.
    std::unique_ptr<DisplaySurface> mPbufferSurface;

    // Used for Composition and Display ops.
    std::unique_ptr<DisplaySurface> mWindowSurface;

    GLint mGlesVersionMajor = 0;
    GLint mGlesVersionMinor = 0;
    GLESDispatchMaxVersion mGlesDispatchMaxVersion = GLES_DISPATCH_MAX_VERSION_2;
    std::string mGlesVendor;
    std::string mGlesRenderer;
    std::string mGlesVersion;
    std::string mGlesExtensions;
    std::optional<GlesUuid> mGlesDeviceUuid;
    bool mGlesVulkanInteropSupported = false;

    std::unique_ptr<EmulatedEglConfigList> mEmulatedEglConfigs;

    bool mFastBlitSupported = false;

    std::unique_ptr<CompositorGl> mCompositorGl;
    std::unique_ptr<DisplayGl> mDisplayGl;
    std::unique_ptr<ReadbackWorkerGl> mReadbackWorkerGl;

    std::unique_ptr<TextureDraw> mTextureDraw;

    PixelReadFormats mPixelReadFormats;

    uint32_t mWidth = 0;
    uint32_t mHeight = 0;

    GlobalState* mGlobalState = nullptr;

    EmulatedEglContextMap mContexts;
    EmulatedEglWindowSurfaceMap mWindows;
    using ProcOwnedEmulatedEglContexts = std::unordered_map<uint64_t, EmulatedEglContextSet>;
    ProcOwnedEmulatedEglContexts mProcOwnedEmulatedEglContexts;
    using ProcOwnedEmulatedEglWindowSurfaces =
        std::unordered_map<uint64_t, EmulatedEglWindowSurfaceSet>;
    ProcOwnedEmulatedEglWindowSurfaces mProcOwnedEmulatedEglWindowSurfaces;
    EmulatedEglImageMap mImages;
    using ProcOwnedEmulatedEglImages = std::unordered_map<uint64_t, EmulatedEglImageSet>;
    ProcOwnedEmulatedEglImages mProcOwnedEmulatedEglImages;

    struct PlatformEglContextInfo {
        EGLContext context;
        EGLSurface surface;
    };
    std::unordered_map<void*, PlatformEglContextInfo> mPlatformEglContexts;

    gfxstream::base::ReadWriteLock mContextStructureLock;
};

}  // namespace gl
}  // namespace host
}  // namespace gfxstream
