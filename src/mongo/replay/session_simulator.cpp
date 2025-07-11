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

#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/replay/replay_command.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <chrono>

namespace mongo {

template <typename Callable>
void handleErrors(Callable&& callable) {
    try {
        // TODO SERVER-106495 will handle properly errors, responses and stats.
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

SessionSimulator::SessionSimulator()
    : _commandExecutor(std::make_unique<ReplayCommandExecutor>()),
      _sessionScheduler(std::make_unique<SessionScheduler>(MAX_SIMULATION_PROCESSING_TASKS)) {}

SessionSimulator::~SessionSimulator() {}

void SessionSimulator::start(StringData uri,
                             std::chrono::steady_clock::time_point replayStartTime,
                             const Date_t& recordStartTime,
                             const Date_t& eventTimestamp) {
    // It safe to pass this (because it will be kept alive by the SessionHandler) and to write or
    // read member variables, because there is only one thread. Beware about spawning multiple
    // threads. Order of commands can be different than the ones recorded and a mutex must be used
    // for supporting multiple threads.
    auto f = [this, uri, replayStartTime, recordStartTime, eventTimestamp]() {
        _replayStartTime = replayStartTime;
        _recordStartTime = recordStartTime;
        // wait if simulation and recording start time have diverged.
        waitIfNeeded(eventTimestamp);
        // connect
        _commandExecutor->connect(uri);
        // set running flag.
        _running = true;
    };

    _sessionScheduler->submit([f]() { handleErrors(f); });
}

void SessionSimulator::stop(const Date_t& sessionEnd) {
    // It safe to pass this (because it will be kept alive by the SessionHandler) and to write or
    // read member variables, because there is only one thread. Beware about spawning multiple
    // threads. Order of commands can be different than the ones recorded and a mutex must be used
    // for supporting multiple threads.
    auto f = [this, sessionEnd]() {
        uassert(ErrorCodes::ReplayClientSessionSimulationError,
                "SessionSimulator is not connected to a valid mongod/s instance.",
                _running);
        waitIfNeeded(sessionEnd);
        _commandExecutor->reset();
    };

    _sessionScheduler->submit([f]() { handleErrors(f); });
}

void SessionSimulator::run(const ReplayCommand& command, const Date_t& commandTimeStamp) const {
    // It safe to pass this (because it will be kept alive by the SessionHandler) and to write or
    // read member variables, because there is only one thread. Beware about spawning multiple
    // threads. Order of commands can be different than the ones recorded and a mutex must be used
    // for supporting multiple threads.
    auto f = [this, command, commandTimeStamp]() {
        uassert(ErrorCodes::ReplayClientSessionSimulationError,
                "SessionSimulator is not connected to a valid mongod/s instance.",
                _running);
        waitIfNeeded(commandTimeStamp);
        _commandExecutor->runCommand(command);
    };

    _sessionScheduler->submit([f]() { handleErrors(f); });
}

std::chrono::steady_clock::time_point SessionSimulator::now() const {
    return std::chrono::steady_clock::now();
}

void SessionSimulator::sleepFor(std::chrono::steady_clock::duration duration) const {
    stdx::this_thread::sleep_for(duration);
}

void SessionSimulator::waitIfNeeded(Date_t eventTimestamp) const {

    // TODO SERVER-106897: Date_t precision is only milliseconds; traffic reader will change the
    // recording format to use "offset from start" for each event with higher precision.
    auto recordingOffset =
        mongo::duration_cast<Microseconds>(eventTimestamp - _recordStartTime).toSystemDuration();

    auto targetTime = _replayStartTime + recordingOffset;
    auto requiredDelay = targetTime - now();
    // wait if needed
    if (requiredDelay > requiredDelay.zero()) {
        sleepFor(requiredDelay);
    }
}
}  // namespace mongo
