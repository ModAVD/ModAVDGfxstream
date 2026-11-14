// Copyright 2022 The Android Open Source Project
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

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace gfxstream {
namespace host {
namespace gl {

// ContextHelper interface class used during Buffer and ColorBuffer
// operations. This is introduced to remove coupling from the FrameBuffer
// class implementation.
class ContextHelper {
  public:
    ContextHelper() = default;
    // When this helper is destroyed, release CPU ownership without issuing GL
    // calls in an unrelated context. Leave native reclamation to EGL teardown.
    enum class CleanupContext { Current, Lost };
    using Cleanup = std::function<void(CleanupContext)>;
    virtual ~ContextHelper() { drainCleanup(CleanupContext::Lost); }
    // Saves the calling thread's current context and makes the helper's
    // context current. Returns false if the context could not be made
    // current; GL calls must then not be made. teardownContext() restores
    // the saved context and is also needed after a failed setupContext().
    virtual bool setupContext() = 0;
    virtual void teardownContext() = 0;
    // Whether this thread is already inside a helper binding scope.
    virtual bool isBound() const = 0;

    bool hasPendingCleanup() const { return mHasCleanup.load(std::memory_order_acquire); }

    size_t deferCleanup(Cleanup cleanup) {
        std::lock_guard<std::mutex> lock(mCleanupMutex);
        mCleanup.emplace_back(std::move(cleanup));
        mHasCleanup.store(true, std::memory_order_release);
        return mCleanup.size();
    }

    size_t drainCleanup(CleanupContext context) {
        if (!hasPendingCleanup()) return 0;
        std::vector<Cleanup> cleanup;
        {
            std::lock_guard<std::mutex> lock(mCleanupMutex);
            cleanup.swap(mCleanup);
            mHasCleanup.store(false, std::memory_order_release);
        }
        for (auto& item : cleanup) item(context);
        return cleanup.size();
    }

   private:
    std::mutex mCleanupMutex;
    std::vector<Cleanup> mCleanup;
    std::atomic<bool> mHasCleanup{false};
};

// Helper class to use a ContextHelper for some set of operations.
// Usage is pretty simple:
//
//     {
//        RecursiveScopedContextBind context(m_helper);
//        if (!context.isOk()) {
//            return false;   // something bad happened.
//        }
//        .... do something ....
//     }   // automatically calls m_helper->teardownContext();
//
class RecursiveScopedContextBind {
  public:
    RecursiveScopedContextBind(ContextHelper* helper) : mHelper(helper) {
        if (helper) {
            const bool nested = helper->isBound();
            mIsBound = helper->setupContext();
            mNeedsTeardown = true;
            // Destructors can change GL bindings. Never run them in the middle
            // of an outer operation, even when setupContext() succeeds.
            if (mIsBound && !nested) helper->drainCleanup(ContextHelper::CleanupContext::Current);
        }
    }

    bool isOk() const { return mIsBound; }

    ~RecursiveScopedContextBind() { release(); }

    void release() {
        if (mNeedsTeardown) {
            mHelper->teardownContext();
            mNeedsTeardown = false;
        }
        mIsBound = false;
    }

  private:
    ContextHelper* mHelper;
    bool mIsBound = false;
    // Also set when setupContext() failed: teardownContext() then restores
    // the context the thread had before.
    bool mNeedsTeardown = false;
};

}  // namespace gl
}  // namespace host
}  // namespace gfxstream
