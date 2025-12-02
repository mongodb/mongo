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

#include "mongo/replay/session_handler.h"

#include "mongo/logv2/log.h"
#include "mongo/replay/performance_reporter.h"

#include <chrono>
#include <exception>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {


void SessionHandler::createSession(key_t key, PacketSource source) {
    auto commandExecutor = std::make_unique<ReplayCommandExecutor>();
    auto perfReporter = std::make_unique<PerformanceReporter>(_uri, _perfFileName);
    auto session = std::make_unique<SessionSimulator>(std::move(source),
                                                      key,
                                                      _replayStartTime,
                                                      _uri,
                                                      std::move(commandExecutor),
                                                      std::move(perfReporter));

    std::thread([session = std::move(session), this] {
        ++_runningSessionCount;
        try {
            session->run(_allSessionStop.get_token());
        } catch (...) {
            auto recordedException = _sessionException.synchronize();
            if (!*recordedException) {
                *recordedException = std::current_exception();
            }
            stopAllSessions();
        }
        --_runningSessionCount;
        notify();
    }).detach();

    LOGV2_DEBUG(10893000, 1, "New Session", "sessionID"_attr = key);
}

void SessionHandler::stopAllSessions() {
    _allSessionStop.request_stop();
}

void SessionHandler::rethrowIfSessionFailed() {
    auto exception = _sessionException.get();
    if (exception) {
        std::rethrow_exception(exception);
    }
}

void SessionHandler::waitForRunningSessions() {
    _cv.wait(_notificationMutex, [&] { return _runningSessionCount == 0; });
}


bool SessionHandler::waitUntil(std::chrono::steady_clock::time_point tp) {
    _cv.wait_until(_notificationMutex, _allSessionStop.get_token(), tp, [&] {
        return _sessionException.get() != nullptr;
    });
    // Return true if the requested time was reached, without an exception or stop request.
    return !_allSessionStop.stop_requested() && _sessionException.get() == nullptr;
}

void SessionHandler::notify() {
    _cv.notify_all();
}
}  // namespace mongo
