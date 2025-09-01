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

#include "mongo/replay/performance_reporter.h"
#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

void SessionHandler::setStartTime(Date_t recordStartTime) {
    _replayStartTime = std::chrono::steady_clock::now();
    _recordStartTime = recordStartTime;
}

void SessionHandler::onSessionStart(Date_t eventTimestamp, int64_t sessionId) {
    addToRunningSessionCache(sessionId);
    // now initialize the session.
    auto& session = getSessionSimulator(sessionId);
    // connects to the server
    session.start(_uri, _replayStartTime, _recordStartTime, eventTimestamp);
}
void SessionHandler::onSessionStart(const ReplayCommand& command) {
    const auto& [ts, sid] = extractTimeStampAndSessionFromCommand(command);
    onSessionStart(ts, sid);
}

void SessionHandler::onSessionStop(const ReplayCommand& stopCommand) {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "Error, failed the command does not represent a stop recording event.",
            stopCommand.isStopRecording());

    const auto& [timestamp, sessionId] = extractTimeStampAndSessionFromCommand(stopCommand);
    auto& session = getSessionSimulator(sessionId);
    session.stop(timestamp);
    // this is correct, because the scheduler will wait until the stop command would have run. In
    // case of errors, the session will need to be deleted either way.
    removeFromRunningSessionCache(sessionId);
}

void SessionHandler::onBsonCommand(const ReplayCommand& command) {
    // just run the command. the Session simulator will make sure things work.
    const auto& [timestamp, sessionId] = extractTimeStampAndSessionFromCommand(command);
    if (!isSessionActive(sessionId)) {
        // TODO SERVER-105627: When session start event will be added remove this code. This is
        // needed for making integration tests pass.
        createNewSessionOnNewCommand(timestamp, sessionId);
    }
    auto& session = getSessionSimulator(sessionId);
    session.run(command, timestamp);
}

void SessionHandler::clear() {
    _runningSessions.clear();
}

void SessionHandler::addToRunningSessionCache(SessionHandler::key_t key) {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "Error, running session cannot contain the same key",
            !isSessionActive(key));

    auto commandExecutor = std::make_unique<ReplayCommandExecutor>();
    auto sessionScheduler = std::make_unique<SessionScheduler>();
    auto perfReporter = std::make_unique<PerformanceReporter>(_uri, _perfFileName);
    auto session = std::make_unique<SessionSimulator>(
        std::move(commandExecutor), std::move(sessionScheduler), std::move(perfReporter));
    _runningSessions.insert({key, std::move(session)});
}

void SessionHandler::removeFromRunningSessionCache(SessionHandler::key_t key) {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "Error, running session must contain the key passed",
            isSessionActive(key));
    _runningSessions.erase(key);
}

SessionSimulator& SessionHandler::getSessionSimulator(SessionHandler::key_t key) {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "Error, running session must contain the key passed",
            isSessionActive(key));
    return *(_runningSessions.at(key));
}

const SessionSimulator& SessionHandler::getSessionSimulator(SessionHandler::key_t key) const {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "Error, running session must contain the key passed",
            isSessionActive(key));
    return *(_runningSessions.at(key));
}

bool SessionHandler::isSessionActive(SessionHandler::key_t key) {
    return _runningSessions.contains(key);
}
bool SessionHandler::isSessionActive(SessionHandler::key_t key) const {
    return _runningSessions.contains(key);
}

void SessionHandler::createNewSessionOnNewCommand(Date_t timestamp, int64_t sessionId) {
    onSessionStart(timestamp, sessionId);
}


}  // namespace mongo
