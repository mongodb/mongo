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

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/replay/options_handler.h"
#include "mongo/replay/recording_reader.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/session_pool.h"
#include "mongo/rpc/factory.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>

using namespace mongo;

stdx::mutex m;
constexpr size_t MAX_PRODUCERS = 1;
constexpr size_t MAX_CONSUMERS = 4;

template <typename Future>
void handleResponse(Future&& fut) {
    try {
        // During the task execution some errors could occur. In this case we catch the error and
        // terminate.
        fut.get();
    } catch (const DBException& ex) {
        tassert(ErrorCodes::ReplayClientInternalError, ex.what(), false);
    } catch (const std::exception& ex) {
        tassert(ErrorCodes::ReplayClientInternalError, ex.what(), false);
    } catch (...) {
        tassert(ErrorCodes::ReplayClientInternalError, "Unknown error type encountered", false);
    }
}

void simpleTask(ReplayCommandExecutor& replayCommandExecutor,
                const std::vector<BSONObj>& bsonCommands) {

    for (const auto& bsonCommand : bsonCommands) {

        ReplayCommand replayCommand{bsonCommand};
        {
            // TODO: SERVER-106046 will make the thread pool session compatible with the simulation
            // requirements.
            stdx::unique_lock<stdx::mutex> lock(m);
            auto response = replayCommandExecutor.runCommand(replayCommand);
        }
    }
}

void threadExecutionFunction(const ReplayOptions& replayOptions) {
    try {
        ReplayCommandExecutor replayCommandExecutor;
        uassert(ErrorCodes::ReplayClientConfigurationError,
                "Failed initializing replay command execution.",
                replayCommandExecutor.init());

        RecordingReader reader{replayOptions.recordingPath};
        const auto commands = reader.parse();

        if (!commands.empty()) {
            replayCommandExecutor.connect(replayOptions.mongoURI);
            // TODO: SERVER-106046 will pin session to worker. For now  MAX_CONSUMERS sessions are
            //       equal to N different consumers/workers serving a session.
            SessionPool sessionPool(MAX_CONSUMERS);
            replayCommandExecutor.connect(replayOptions.mongoURI);
            std::vector<stdx::future<void>> futures;

            for (size_t i = 0; i < MAX_PRODUCERS; ++i) {
                auto f = sessionPool.submit(simpleTask, std::ref(replayCommandExecutor), commands);
                futures.push_back(std::move(f));
            }

            for (auto& f : futures) {
                handleResponse(std::move(f));
            }
        }

    } catch (const std::exception& e) {
        tassert(ErrorCodes::ReplayClientInternalError, e.what(), false);
    }
}

int main(int argc, char** argv) {

    OptionsHandler commandLineOptions;
    const auto& options = commandLineOptions.handle(argc, argv);

    std::vector<stdx::thread> instances;
    for (size_t i = 0; i < options.size(); ++i) {
        instances.push_back(stdx::thread(threadExecutionFunction, std::ref(options[i])));
    }

    for (auto& instance : instances) {
        instance.join();
    }

    return 0;
}
