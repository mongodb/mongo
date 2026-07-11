// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/replay/session_simulator.h"
#include "mongo/util/modules.h"
#include "mongo/util/synchronized_value.h"

#include <chrono>
#include <condition_variable>
#include <exception>

namespace mongo {
class ReplayCommand;
class SessionSimulator;
class SessionHandler {
public:
    using key_t = int64_t;

    /**
     * Enable perf recording. Essentially record each command and how long it took plus the
     * response. By default enable perf recording is disabled (useful for testing). But for real
     * simulations the recording will always be enabled.
     */
    explicit SessionHandler(
        std::string uri,
        std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now(),
        std::string perfFileName = "")
        : _replayStartTime(startTime),
          _uri(std::move(uri)),
          _perfFileName(std::move(perfFileName)) {}

    void createSession(key_t sid, PacketSource source);
    /**
     * Return number of sessions running
     */
    size_t fetchTotalRunningSessions() const {
        return _runningSessionCount.load();
    }

    void stopAllSessions();

    /**
     * Re-throw exception captured from a failed session.
     */
    void rethrowIfSessionFailed();

    /**
     * Wait for any remaining session replays to end, or for stop to be requested.
     *
     */
    void waitForRunningSessions();

    /**
     * Wait until the provided timepoint, stop is requested by the parent, or an exception is thrown
     * by a session replay.
     */
    bool waitUntil(std::chrono::steady_clock::time_point tp);

    void notify();

private:
    mongo::stop_source _allSessionStop;

    std::mutex _notificationMutex;  // NOLINT
    std::condition_variable _cv;    // NOLINT

    std::atomic<int64_t> _runningSessionCount = 0;  // NOLINT

    mongo::synchronized_value<std::exception_ptr> _sessionException;

    std::chrono::steady_clock::time_point _replayStartTime;  // when the replay started

    std::string _uri;           // uri of the mongo shadow instance
    std::string _perfFileName;  // perf recording file name if specified
};
}  // namespace mongo
