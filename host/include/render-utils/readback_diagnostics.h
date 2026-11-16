// Copyright 2026 Ji Mao Cai
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "gfxstream/common/logging.h"

namespace gfxstream {
namespace host {

// Counts requested/read-back bytes, never driver allocations or leaked bytes.
// HostFrame counts callback submissions, not reads by a shared-memory consumer.
enum class ReadbackSource {
    Guest,
    GuestDma,
    HostScreenshot,
    HostQuery,
    HostFrame,
    GlPixels,
    GlScaled,
    Count
};
enum class ReadbackOutcome { Returned, Failed };
enum class DiagnosticEvent {
    CleanupDeferred,
    CleanupRecovered,
    CleanupAbandoned,
    DmaMissingFramebuffer,
    EglContextCpuOnly,
    EglContextRestoreFailed,
    Count
};

class ReadbackDiagnostics {
   public:
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)();
    explicit ReadbackDiagnostics(Now now = Clock::now)
        : mNow(now), mLastLog(now() - std::chrono::minutes(1)) {}

    static ReadbackDiagnostics& get() {
        static ReadbackDiagnostics diagnostics;
        return diagnostics;
    }

    void readback(ReadbackSource source, int width, int height, uint64_t bytes, uint64_t elapsedUs,
                  ReadbackOutcome outcome, uint32_t handle = 0, uint64_t puid = 0) {
        bool logFailure = false;
        uint64_t failures = 0;
        std::optional<Snapshot> summary;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto& row = mReadbacks[static_cast<size_t>(source)];
            ++row.calls;
            row.bytes += bytes;
            row.elapsedUs += elapsedUs;
            row.maxUs = std::max(row.maxUs, elapsedUs);
            row.width = width;
            row.height = height;
            row.handle = handle;
            row.puid = puid;
            if (outcome == ReadbackOutcome::Failed) {
                failures = ++row.failures;
                logFailure = row.failureDetails.take(mNow());
            }
            summary = snapshotLocked();
        }
        // Logging callbacks must be free to query/update diagnostics themselves.
        // Only the first three failures per source per minute get detailed logs.
        if (logFailure) {
            GFXSTREAM_ERROR(
                "MAS: readback failed source=%s size=%dx%d bytes=%llu elapsedUs=%llu "
                "cb=%u puid=%llu failures=%llu",
                name(source), width, height, static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(elapsedUs), handle,
                static_cast<unsigned long long>(puid), static_cast<unsigned long long>(failures));
        }
        logSummary(summary);
    }

    bool shouldLog(DiagnosticEvent event) {
        std::lock_guard<std::mutex> lock(mMutex);
        return mEventLogs[static_cast<size_t>(event)].take(mNow());
    }

    void colorBufferCreated(uint64_t rgbaBytes) {
        std::optional<Snapshot> summary;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mResources.created;
            mResources.liveRgba += rgbaBytes;
            ++mResourceRevision;
            summary = snapshotLocked();
        }
        logSummary(summary);
    }
    void colorBufferReleased(uint64_t rgbaBytes) {
        std::lock_guard<std::mutex> lock(mMutex);
        ++mResources.released;
        mResources.liveRgba -= rgbaBytes;
        ++mResourceRevision;
    }
    void cleanupDeferred(uint64_t rgbaBytes) {
        std::lock_guard<std::mutex> lock(mMutex);
        ++mResources.deferred;
        mResources.pendingRgba += rgbaBytes;
        ++mResourceRevision;
    }
    void cleanupFinished(uint64_t rgbaBytes, bool recovered) {
        std::optional<Snapshot> summary;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (recovered)
                ++mResources.recovered;
            else
                ++mResources.abandoned;
            mResources.pendingRgba -= rgbaBytes;
            ++mResourceRevision;
            summary = snapshotLocked();
        }
        logSummary(summary);
    }
    void renderThreadCreated() {
        std::lock_guard<std::mutex> lock(mMutex);
        ++mResources.threadsCreated;
        ++mResourceRevision;
    }
    void eglContextCpuOnly() {
        std::optional<Snapshot> summary;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mResources.eglContextCpuOnly;
            ++mResourceRevision;
            summary = snapshotLocked();
        }
        logSummary(summary);
    }
    void eglContextRestoreFailed() {
        std::optional<Snapshot> summary;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mResources.eglContextRestoreFailures;
            ++mResourceRevision;
            summary = snapshotLocked();
        }
        logSummary(summary);
    }
    void renderThreadReleased() {
        std::optional<Snapshot> summary;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mResources.threadsReleased;
            ++mResourceRevision;
            summary = snapshotLocked();
        }
        logSummary(summary);
    }
    void log() {
        std::optional<Snapshot> summary;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            summary = snapshotLocked(true);
        }
        logSummary(summary);
    }

   private:
    static constexpr size_t kSourceCount = static_cast<size_t>(ReadbackSource::Count);
    struct LogBudget {
        Clock::time_point window{};
        unsigned reported = 0;
        bool take(Clock::time_point now) {
            if (now - window >= std::chrono::minutes(1)) {
                window = now;
                reported = 0;
            }
            if (reported == 3) return false;
            ++reported;
            return true;
        }
    };
    struct Readbacks {
        uint64_t calls = 0, bytes = 0, elapsedUs = 0, maxUs = 0, failures = 0;
        uint64_t loggedCalls = 0;
        int width = 0, height = 0;
        uint32_t handle = 0;
        uint64_t puid = 0;
        LogBudget failureDetails;
    };
    struct Resources {
        uint64_t created = 0, released = 0, liveRgba = 0;
        uint64_t deferred = 0, recovered = 0, abandoned = 0, pendingRgba = 0;
        uint64_t threadsCreated = 0, threadsReleased = 0;
        uint64_t eglContextCpuOnly = 0;
        uint64_t eglContextRestoreFailures = 0;
    };
    struct Snapshot {
        std::array<Readbacks, kSourceCount> rows{};
        std::array<bool, kSourceCount> changed{};
        Resources resources;
        bool resourcesChanged = false;
    };
    static const char* name(ReadbackSource source) {
        switch (source) {
            case ReadbackSource::Guest:
                return "guest";
            case ReadbackSource::GuestDma:
                return "guest-dma";
            case ReadbackSource::HostScreenshot:
                return "host-screenshot";
            case ReadbackSource::HostQuery:
                return "host-size-query";
            case ReadbackSource::HostFrame:
                return "host-frame-submit";
            case ReadbackSource::GlPixels:
                return "gl-pixels";
            case ReadbackSource::GlScaled:
                return "gl-scaled";
            case ReadbackSource::Count:
                break;
        }
        return "invalid";
    }
    std::optional<Snapshot> snapshotLocked(bool force = false) {
        const auto now = mNow();
        if (!force && now - mLastLog < std::chrono::minutes(1)) return std::nullopt;
        mLastLog = now;
        Snapshot snapshot;
        for (size_t i = 0; i < mReadbacks.size(); ++i) {
            auto& row = mReadbacks[i];
            if (row.calls == row.loggedCalls) continue;
            snapshot.rows[i] = row;
            snapshot.changed[i] = true;
            row.loggedCalls = row.calls;
        }
        snapshot.resources = mResources;
        snapshot.resourcesChanged = mResourceRevision != mLoggedResourceRevision;
        mLoggedResourceRevision = mResourceRevision;
        return snapshot;
    }
    static void logSummary(const std::optional<Snapshot>& snapshot) {
        if (!snapshot) return;
        for (size_t i = 0; i < snapshot->rows.size(); ++i) {
            if (!snapshot->changed[i]) continue;
            const auto& row = snapshot->rows[i];
            GFXSTREAM_INFO(
                "MAS: readback source=%s calls=%llu bytes=%llu failures=%llu "
                "totalUs=%llu maxUs=%llu lastSize=%dx%d lastCb=%u lastPuid=%llu",
                name(static_cast<ReadbackSource>(i)), static_cast<unsigned long long>(row.calls),
                static_cast<unsigned long long>(row.bytes),
                static_cast<unsigned long long>(row.failures),
                static_cast<unsigned long long>(row.elapsedUs),
                static_cast<unsigned long long>(row.maxUs), row.width, row.height, row.handle,
                static_cast<unsigned long long>(row.puid));
        }
        if (!snapshot->resourcesChanged) return;
        const auto& resources = snapshot->resources;
        GFXSTREAM_INFO(
            "MAS: readback resources cbCreated=%llu cbReleased=%llu "
            "cbRgbaEstimateBytes=%llu cleanupDeferred=%llu cleanupRecovered=%llu "
            "cleanupAbandoned=%llu pendingRgbaEstimateBytes=%llu "
            "threadsCreated=%llu threadsReleased=%llu eglContextCpuOnly=%llu "
            "eglContextRestoreFailures=%llu",
            static_cast<unsigned long long>(resources.created),
            static_cast<unsigned long long>(resources.released),
            static_cast<unsigned long long>(resources.liveRgba),
            static_cast<unsigned long long>(resources.deferred),
            static_cast<unsigned long long>(resources.recovered),
            static_cast<unsigned long long>(resources.abandoned),
            static_cast<unsigned long long>(resources.pendingRgba),
            static_cast<unsigned long long>(resources.threadsCreated),
            static_cast<unsigned long long>(resources.threadsReleased),
            static_cast<unsigned long long>(resources.eglContextCpuOnly),
            static_cast<unsigned long long>(resources.eglContextRestoreFailures));
    }

    std::mutex mMutex;
    Now mNow;
    std::array<LogBudget, static_cast<size_t>(DiagnosticEvent::Count)> mEventLogs{};
    std::array<Readbacks, kSourceCount> mReadbacks{};
    Clock::time_point mLastLog;
    Resources mResources;
    uint64_t mResourceRevision = 0, mLoggedResourceRevision = 0;
};

class ReadbackScope {
   public:
    ReadbackScope(ReadbackSource source, int width, int height, uint64_t bytes,
                  ReadbackOutcome outcome = ReadbackOutcome::Returned)
        : mSource(source), mWidth(width), mHeight(height), mBytes(bytes), mOutcome(outcome) {}
    ReadbackScope(const ReadbackScope&) = delete;
    ReadbackScope& operator=(const ReadbackScope&) = delete;
    ~ReadbackScope() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 ReadbackDiagnostics::Clock::now() - mStart)
                                 .count();
        ReadbackDiagnostics::get().readback(mSource, mWidth, mHeight, mBytes,
                                            static_cast<uint64_t>(elapsed), mOutcome, mHandle,
                                            mPuid);
    }
    void result(bool ok) { mOutcome = ok ? ReadbackOutcome::Returned : ReadbackOutcome::Failed; }
    void bytes(uint64_t bytes) { mBytes = bytes; }
    void dimensions(int width, int height) {
        mWidth = width;
        mHeight = height;
    }
    void identity(uint32_t handle, uint64_t puid = 0) {
        mHandle = handle;
        mPuid = puid;
    }

   private:
    const ReadbackSource mSource;
    int mWidth, mHeight;
    uint64_t mBytes;
    ReadbackOutcome mOutcome;
    uint32_t mHandle = 0;
    uint64_t mPuid = 0;
    const ReadbackDiagnostics::Clock::time_point mStart = ReadbackDiagnostics::Clock::now();
};

}  // namespace host
}  // namespace gfxstream
