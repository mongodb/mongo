/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include "mongo/replay/session_simulator.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/synchronized_value.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <stop_token>
#include <thread>

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
    std::stop_source _allSessionStop;

    std::mutex _notificationMutex;    // NOLINT
    std::condition_variable_any _cv;  // NOLINT

    std::atomic<int64_t> _runningSessionCount = 0;  // NOLINT

    mongo::synchronized_value<std::exception_ptr> _sessionException;

    std::chrono::steady_clock::time_point _replayStartTime;  // when the replay started

    std::string _uri;           // uri of the mongo shadow instance
    std::string _perfFileName;  // perf recording file name if specified
};
}  // namespace mongo
