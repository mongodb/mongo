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

#include "mongo/replay/replay_client.h"

#include "mongo/db/traffic_reader.h"
#include "mongo/replay/recording_reader.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_config.h"
#include "mongo/replay/session_handler.h"
#include "mongo/stdx/future.h"
#include "mongo/util/assert_util.h"

#include <exception>
#include <string>

namespace mongo {

void replayThread(const ReplayConfig& replayConfig) {
    try {

        RecordingReader reader{replayConfig.recordingPath};
        const auto bsonRecordedCommands = reader.processRecording();

        uassert(ErrorCodes::ReplayClientInternalError,
                "The list of recorded commands cannot be empty",
                !bsonRecordedCommands.empty());

        // create a new session handler for mananging the recording.
        SessionHandler sessionHandler;

        // setup recording and replaying starting time
        auto firstCommand = bsonRecordedCommands[0];
        sessionHandler.setStartTime(ReplayCommand{firstCommand}.fetchRequestTimestamp());

        for (const auto& bsonCommand : bsonRecordedCommands) {
            ReplayCommand command{bsonCommand};
            if (command.isStartRecording()) {
                // will associated the URI to a session task and run all the commands associated
                // with this session id.
                sessionHandler.onSessionStart(replayConfig.mongoURI, command);
            } else if (command.isStopRecording()) {
                // stop commad will reset the complete the simulation and reset the connection.
                sessionHandler.onSessionStop(command);
            } else {
                // must be a runnable command.
                sessionHandler.onBsonCommand(replayConfig.mongoURI, command);
            }
        }

    } catch (const DBException& ex) {
        // If we have reached this point we have encountered a problem in the recording. Either a
        // ill recording file or some connectivity issue.
        // TODO SERVER-106495: report and record these errors.
        tasserted(ErrorCodes::ReplayClientSessionSimulationError,
                  "DBException in handleAsyncResponse, terminating due to:" + ex.toString());
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientInternalError, e.what());
    } catch (...) {
        tasserted(ErrorCodes::ReplayClientInternalError, "Unknown error.");
    }
}

void ReplayClient::replayRecording(const ReplayConfigs& configs) {
    std::vector<stdx::thread> instances;
    for (const auto& config : configs) {
        instances.push_back(stdx::thread(replayThread, std::ref(config)));
    }
    for (auto& instance : instances) {
        if (instance.joinable()) {
            instance.join();
        }
    }
}

void ReplayClient::replayRecording(const std::string& recordingFileName, const std::string& uri) {
    ReplayConfig config{recordingFileName, uri};
    replayRecording({config});
}

}  // namespace mongo
