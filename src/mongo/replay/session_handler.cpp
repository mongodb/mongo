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

#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

void SessionHandler::setStartTime(Date_t recordStartTime) {
    _replayStartTime = std::chrono::steady_clock::now();
    _recordStartTime = recordStartTime;
}

void SessionHandler::onSessionStart(StringData uri, const ReplayCommand& startCommand) {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "Error, failed the command does not represent a start recording event.",
            startCommand.isStartRecording());

    const auto& [eventTimestamp, sessionId] = extractTimeStampAndSessionFromCommand(startCommand);
    addToRunningSessionCache(sessionId);
    // now initialize the session.
    auto& session = getSessionSimulator(sessionId);
    // connects to the server
    session.start(uri, _replayStartTime, _recordStartTime, eventTimestamp);
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

void SessionHandler::onBsonCommand(StringData uri, const ReplayCommand& command) {
    // just run the command. the Session simulator will make sure things work.
    const auto& [timestamp, sessionId] = extractTimeStampAndSessionFromCommand(command);
    if (!isSessionActive(sessionId)) {
        // TODO SERVER-105627: When session start event will be added remove this code. This is
        // needed for making integration tests pass.
        createNewSessionOnNewCommand(uri, sessionId);
    }
    const auto& session = getSessionSimulator(sessionId);
    session.run(command, timestamp);
}

void SessionHandler::clear() {
    _runningSessions.clear();
}

void SessionHandler::addToRunningSessionCache(SessionHandler::key_t key) {
    uassert(ErrorCodes::ReplayClientSessionSimulationError,
            "Error, running session cannot contain the same key",
            !isSessionActive(key));
    _runningSessions.insert({key, std::make_shared<SessionSimulator>()});
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

std::pair<Date_t, int64_t> SessionHandler::extractTimeStampAndSessionFromCommand(
    const ReplayCommand& command) const {
    const Date_t timestamp = command.fetchRequestTimestamp();
    const int64_t sessionId = command.fetchRequestSessionId();
    return {timestamp, sessionId};
}

bool SessionHandler::isSessionActive(SessionHandler::key_t key) {
    return _runningSessions.contains(key);
}
bool SessionHandler::isSessionActive(SessionHandler::key_t key) const {
    return _runningSessions.contains(key);
}

void SessionHandler::createNewSessionOnNewCommand(StringData uri, int64_t sessionId) {
    // TODO SERVER-105627: This is simulating a start traffic recording event. Because right now we
    // don't have such event in the recording. It needs to be simulated until we won't provide a
    // proper event. Otherwise no recording could be played.
    BSONObj startRecording =
        BSON("startTrafficRecording"
             << "1.0" << "destination" << "rec" << "lsid"
             << BSON("id" << "UUID(\"a8ac2bdc-5457-4a86-9b1c-b0a3253bc43e\")") << "$db" << "admin");
    RawOpDocument opDoc{"startTrafficRecording", startRecording};
    opDoc.updateSeenField(Date_t::now());
    opDoc.updateSessionId(sessionId);
    ReplayCommand commandStart{opDoc.getDocument()};
    onSessionStart(uri, commandStart);
}


}  // namespace mongo
