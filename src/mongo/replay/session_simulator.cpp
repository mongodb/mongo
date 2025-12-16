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

#include "mongo/replay/session_simulator.h"

#include "mongo/db/query/util/stop_token.h"
#include "mongo/logv2/log.h"
#include "mongo/replay/replay_command.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"

#include <chrono>
#include <exception>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault
namespace mongo {

template <typename Callable>
void handleErrors(Callable&& callable) {
    try {
        callable();
    } catch (const DBException& ex) {
        tasserted(ErrorCodes::ReplayClientSessionSimulationError,
                  "DBException in handleAsyncResponse, terminating due to:" + ex.toString());
    } catch (const std::exception& ex) {
        tasserted(ErrorCodes::ReplayClientSessionSimulationError,
                  "Exception in handleAsyncResponse, terminating due to:" + std::string{ex.what()});
    } catch (...) {
        tasserted(ErrorCodes::ReplayClientSessionSimulationError,
                  "Unknown exception in handleAsyncResponse, terminating");
    }
}

static constexpr size_t MAX_SIMULATION_PROCESSING_TASKS = 1;

SessionSimulator::SessionSimulator(PacketSource source,
                                   uint64_t sessionID,
                                   std::chrono::steady_clock::time_point globalStartTime,
                                   std::string uri,
                                   std::unique_ptr<ReplayCommandExecutor> replayCommandExecutor,
                                   std::unique_ptr<PerformanceReporter> perfReporter)
    : _replayStartTime(globalStartTime),
      _uri(uri),
      _source(std::move(source)),
      _sessionID(sessionID),
      _commandExecutor(std::move(replayCommandExecutor)),
      _perfReporter(std::move(perfReporter)) {}

// We don't want to replay all the commands we find in the recording file. Mainly we want to skip:
// 1. legacy commands. Everything that is marked legacy, won't be replayable.
// 2. Responses (cursor). These will be the result of some query like find and aggregate.
// 3. n/ok commands. These are just responses.
// 4. isWritablePrimary/isMaster. These are mostly diagnostic commands that we don't want.
// NOLINTNEXTLINE needs audit
static const std::unordered_set<std::string> forbiddenKeywords{"legacy",
                                                               "cursor",
                                                               "endSessions",
                                                               "ok",
                                                               "isWritablePrimary",
                                                               "n",
                                                               "isMaster",
                                                               "ismaster",
                                                               "stopTrafficRecording"};
bool isReplayable(const std::string& commandType) {
    return !commandType.empty() && !forbiddenKeywords.contains(commandType);
}

void SessionSimulator::run(mongo::stop_token stopToken) {
    LOGV2_DEBUG(10893001, 1, "Session execution started");
    auto onFail = ScopeGuard([] { LOGV2_ERROR(10893003, "Session execution failed"); });
    bool stopEventSeen = false;
    for (const auto& packet : _source) {
        if (stopToken.stop_requested()) {
            LOGV2_WARNING(10893004, "Session execution halted");
            onFail.dismiss();
            return;
        }
        ReplayCommand command{packet};
        if (!isReplayable(command.parseOpType())) {
            continue;
        }

        const auto& [offset, sessionId] = extractOffsetAndSessionFromCommand(command);

        if (sessionId != _sessionID) {
            continue;
        }

        waitIfNeeded(offset);

        if (command.isSessionStart()) {
            start();
            continue;
        }

        // TODO SERVER-105627: Until session start events are recorded, treat the first observed
        // command as starting the session.
        if (!_running) {
            start();
        }

        if (command.isSessionEnd()) {
            stop();
            stopEventSeen = true;
            break;
        }

        // must be a runnable command.
        runCommand(command);
    }
    if (!stopEventSeen) {
        // TODO: SERVER-111903 strengthen this to a uassert once session end events
        // are guaranteed to be observed at recording end.
        LOGV2_WARNING(10893011,
                      "Recording exhausted without observing session end",
                      "sessionID"_attr = _sessionID);
    }
    onFail.dismiss();
    LOGV2_DEBUG(10893002, 1, "Session execution completed");
}

void SessionSimulator::start() {
    // connect
    _commandExecutor->connect(_uri);
    // set running flag.
    _running = true;
}

void SessionSimulator::stop() {
    // It safe to pass this (because it will be kept alive by the SessionHandler) and to write or
    // read member variables, because there is only one thread. Beware about spawning multiple
    // threads. Order of commands can be different than the ones recorded and a mutex must be used
    // for supporting multiple threads.
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "SessionSimulator is not connected to a valid mongod/s instance.",
            _running);
}

void SessionSimulator::runCommand(const ReplayCommand& command) const {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "SessionSimulator is not connected to a valid mongod/s instance.",
            _running);
    _perfReporter->executeAndRecordPerf(
        [this](const ReplayCommand& command) { return _commandExecutor->runCommand(command); },
        command);
}

std::chrono::steady_clock::time_point SessionSimulator::now() const {
    return std::chrono::steady_clock::now();
}

void SessionSimulator::sleepFor(std::chrono::steady_clock::duration duration) const {
    stdx::this_thread::sleep_for(duration);
}

void SessionSimulator::waitIfNeeded(Microseconds recordingOffset) const {

    LOGV2_DEBUG(
        1232304, 2, "Session waiting until offset", "offset"_attr = recordingOffset.toString());
    auto targetTime = _replayStartTime + recordingOffset.toSystemDuration();
    auto requiredDelay = targetTime - now();
    // wait if needed
    if (requiredDelay > requiredDelay.zero()) {
        sleepFor(requiredDelay);
    }
}
}  // namespace mongo
