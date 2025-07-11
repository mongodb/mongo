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

#include "mongo/base/string_data.h"
#include "mongo/replay/session_simulator.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <memory>

namespace mongo {
class ReplayCommand;
class SessionSimulator;
class SessionHandler {
public:
    using key_t = int64_t;
    /* Global start time shared with all the sessions*/
    void setStartTime(Date_t recordStartTime);
    /**
     * Start a new session given uri and start session recorded command. Returns the key for the
     * session just started
     */
    void onSessionStart(StringData, const ReplayCommand&);
    /**
     * Stop the session started with the key provided as argument and use the stop command received
     */
    void onSessionStop(const ReplayCommand&);
    /**
     * Just replay the command, read from the recording file
     */
    void onBsonCommand(StringData, const ReplayCommand&);
    /**
     * To use carefully, basically destroys all the sessions and reset the session cache
     */
    void clear();
    /**
     * Return number of sessions running
     */
    size_t fetchTotalRunningSessions() const {
        return _runningSessions.size();
    }

private:
    stdx::unordered_map<key_t, std::shared_ptr<SessionSimulator>> _runningSessions;
    std::chrono::steady_clock::time_point _replayStartTime;  // when the replay started
    Date_t _recordStartTime;                                 // timestamp of first event.

    void addToRunningSessionCache(key_t);
    void removeFromRunningSessionCache(key_t);

    bool isSessionActive(int64_t);
    bool isSessionActive(int64_t) const;

    SessionSimulator& getSessionSimulator(key_t);
    const SessionSimulator& getSessionSimulator(key_t) const;

    std::pair<Date_t, int64_t> extractTimeStampAndSessionFromCommand(const ReplayCommand&) const;

    void createNewSessionOnNewCommand(StringData, int64_t);
};
}  // namespace mongo
