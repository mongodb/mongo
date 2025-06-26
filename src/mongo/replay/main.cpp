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

int main(int argc, char** argv) {
    try {
        auto options = OptionsHandler::handle(argc, argv);
        uassert(ErrorCodes::ReplayClientConfigurationError,
                "Failed parsing command line options.",
                options);

        ReplayCommandExecutor replayCommandExecutor;
        uassert(ErrorCodes::ReplayClientConfigurationError,
                "Failed initializing replay command execution.",
                replayCommandExecutor.init());

        RecordingReader reader{options.inputFile};
        const auto commands = reader.parse();

        if (!commands.empty()) {

            replayCommandExecutor.connect(options.mongoURI);
            // 4 sessions are equal to 4 different consumers in this first implementation.
            SessionPool sessionPool(MAX_CONSUMERS);
            std::vector<stdx::future<void>> futures;

            for (size_t i = 0; i < MAX_PRODUCERS; ++i) {
                auto f = sessionPool.submit(simpleTask, std::ref(replayCommandExecutor), commands);
                futures.push_back(std::move(f));
            }

            for (auto& f : futures) {
                f.get();
            }
        }

    } catch (const std::exception& e) {
        tassert(ErrorCodes::ReplayClientInternalError, e.what(), false);
    }

    return 0;
}
