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

#include "mongo/db/query/util/stop_token.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_config.h"
#include "mongo/replay/session_handler.h"
#include "mongo/replay/traffic_recording_iterator.h"
#include "mongo/util/assert_util.h"

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace mongo {
/**
 * Helper class for applying a callable to a container of N elements,
 * spawning a new thread for each element.
 *
 * e.g.,
 *     std::vector<Task> vec{...};
 *     ParallelExecutor::apply(vec, [](token stop_token, auto value) {
 *          // Do something on a separate thread per task.
 *     });
 *
 * Tasks receive a stop token, and will be requested to stop after any
 * task throws an exception. Once all tasks have finished, either the
 * apply call will return, or an exception from one of the tasks
 * will be re-thrown.
 *
 */
class ParallelExecutor {
public:
    ParallelExecutor() = default;
    ParallelExecutor(const ParallelExecutor&) = delete;
    ParallelExecutor(ParallelExecutor&&) = delete;

    ParallelExecutor& operator=(const ParallelExecutor&) = delete;
    ParallelExecutor& operator=(ParallelExecutor&&) = delete;

    /**
     * Apply a callable to each element of a container in a separate thread, passing the provided
     * args to each.
     *
     * If any invocation results in an exception, signal all threads to stop, wait for them to exit,
     * then rethrow the exception.
     */
    static void apply(auto container, auto callable) {
        const auto taskCount = std::size(container);
        std::vector<stdx::thread> instances;
        instances.reserve(taskCount);

        ParallelExecutor state;

        for (auto&& task : container) {
            state.started();
            instances.push_back(stdx::thread([&]() {
                try {
                    callable(state.stop.get_token(), task);
                    state.success();
                } catch (...) {
                    state.fail(std::current_exception());
                }
            }));
        }

        state.wait();

        for (auto& instance : instances) {
            if (instance.joinable()) {
                instance.join();
            }
        }

        state.maybeRethrow();
    }

private:
    void started() {
        auto lh = std::unique_lock(lock);
        ++running;
    }

    void success() {
        auto lh = std::unique_lock(lock);
        --running;
        cv.notify_all();
    }

    void fail(std::exception_ptr ptr) {
        auto lh = std::unique_lock(lock);
        --running;
        if (!exception) {
            exception = ptr;
        }
        cv.notify_all();
    }

    /**
     * Wait until all threads exit.
     *
     * If any thread ends with an exception, signal all to stop, wait for all to exit.
     */
    void wait() {
        auto lh = std::unique_lock(lock);

        // Wait until an exception is reported, or all threads finish.
        cv.wait(lh, [&]() { return exception || running == 0; });
        if (exception) {
            stop.request_stop();
        }
        // Wait for all threads to finish
        cv.wait(lh, [&]() { return running == 0; });
    }

    void maybeRethrow() {
        if (exception) {
            std::rethrow_exception(exception);
        }
    }


    mongo::stop_source stop;

    std::mutex lock;
    std::condition_variable cv;  // NOLINT
    size_t running = 0;
    std::exception_ptr exception = nullptr;
};

// NOLINTNEXTLINE needs audit
static std::unordered_set<std::string> forbiddenKeywords{
    "legacy", "cursor", "endSessions", "ok", "isWritablePrimary", "n"};

bool isReplayable(const std::string& commandType) {
    return !commandType.empty() && !forbiddenKeywords.contains(commandType);
}

/**
 * Consumes a collection of recording files from a _single_ node.
 *
 * Handles creation of threads to replay individual sessions contained within.
 */
void recordingDispatcher(mongo::stop_token stop, const ReplayConfig& replayConfig) {
    std::shared_ptr<FileSet> files;
    try {
        files = FileSet::from_directory(replayConfig.recordingPath);
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::FileOpenFailed, e.what());
    }

    try {
        auto iter = RecordingSetIterator(files);

        if (iter == end(iter)) {
            // There are no events in the recording.
            return;
        }

        // setup recording and replaying starting time
        auto firstCommand = *iter;
        // create a new session handler for mananging the recording.
        SessionHandler sessionHandler;
        sessionHandler.setStartTime(ReplayCommand{firstCommand}.fetchRequestTimestamp());

        for (const auto& packet : iter) {
            if (stop.stop_requested()) {
                return;
            }
            ReplayCommand command{packet};
            if (!isReplayable(command.parseOpType())) {
                continue;
            }
            if (command.isStartRecording()) {
                // will associated the URI to a session task and run all the commands associated
                // with this session id.
                const auto& [timestamp, sessionId] = extractTimeStampAndSessionFromCommand(command);
                sessionHandler.onSessionStart(replayConfig.mongoURI, timestamp, sessionId);
            } else if (command.isStopRecording()) {
                // stop commad will reset the complete the simulation and reset the connection.
                sessionHandler.onSessionStop(command);
            } else {
                // must be a runnable command.
                sessionHandler.onBsonCommand(replayConfig.mongoURI, command);
            }
        }
    } catch (const std::exception& e) {
        tasserted(ErrorCodes::ReplayClientInternalError, e.what());
    }
}

void ReplayClient::replayRecording(const ReplayConfigs& configs) {
    ParallelExecutor::apply(configs, recordingDispatcher);
}

void ReplayClient::replayRecording(const std::string& recordingFileName, const std::string& uri) {
    ReplayConfig config{recordingFileName, uri};
    replayRecording({config});
}

}  // namespace mongo
