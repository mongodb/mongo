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

#include "mongo/replay/replay_command.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <chrono>

namespace mongo {

void SessionSimulator::start(StringData uri,
                             std::chrono::steady_clock::time_point replayStart,
                             Date_t recordingStart,
                             Date_t sessionStart) {
    _replayStartTime = replayStart;
    // beginning of recording must be used a ref time for the recording
    _recordingStartTime = recordingStart;
    // wait if simulation and recording start time have diverged.
    waitIfNeeded(sessionStart);
    // connect
    _commandExecutor.connect(uri);
}

void SessionSimulator::stop(Date_t time) {
    waitIfNeeded(time);
    _commandExecutor.reset();
}

BSONObj SessionSimulator::run(const ReplayCommand& command) const {
    Date_t time = command.fetchRequestTimestamp();
    waitIfNeeded(time);
    return _commandExecutor.runCommand(command);
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
        mongo::duration_cast<Microseconds>(eventTimestamp - _recordingStartTime).toSystemDuration();

    auto targetTime = _replayStartTime + recordingOffset;
    auto requiredDelay = targetTime - now();
    // wait if needed
    if (requiredDelay > requiredDelay.zero()) {
        sleepFor(requiredDelay);
    }
}
}  // namespace mongo
