// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/session_handler.h"

#include "mongo/db/query/util/stop_token.h"
#include "mongo/logv2/log.h"
#include "mongo/replay/performance_reporter.h"

#include <chrono>
#include <exception>
#include <mutex>

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
    ++_runningSessionCount;
    std::thread([session = std::move(session), this] {
        try {
            session->run(_allSessionStop.get_token());
        } catch (...) {
            auto recordedException = _sessionException.synchronize();
            if (!*recordedException) {
                *recordedException = std::current_exception();
            }
            stopAllSessions();
        }
        std::unique_lock ul(_notificationMutex);
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
    std::unique_lock ul(_notificationMutex);
    _cv.wait(ul, [&] { return _runningSessionCount == 0; });
}


bool SessionHandler::waitUntil(std::chrono::steady_clock::time_point tp) {
    std::unique_lock ul(_notificationMutex);
    // std::stop_token not supported on all toolchains, cannot use
    // condition_variable_any::wait* overloads which take std::stop_token.
    // Reproduce behaviour with a mongo::stop_callback.
    mongo::stop_callback sc(_allSessionStop.get_token(), [&] { _cv.notify_all(); });
    _cv.wait_until(ul, tp, [&] {
        return _sessionException.get() != nullptr || _allSessionStop.stop_requested();
    });
    // Return true if the requested time was reached, without an exception or stop request.
    return !_allSessionStop.stop_requested() && _sessionException.get() == nullptr;
}

void SessionHandler::notify() {
    _cv.notify_all();
}
}  // namespace mongo
