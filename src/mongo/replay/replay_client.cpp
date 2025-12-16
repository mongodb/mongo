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
#include "mongo/logv2/log.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_config.h"
#include "mongo/replay/session_handler.h"
#include "mongo/replay/traffic_recording_iterator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

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
        /**
         * Begin reading the provided recording, searching for session starts.
         *
         * Upon finding a session start, spawn a new thread to manage that session.
         * That thread will replay further events for that session at the appropriate time.
         *
         */
        auto iter = RecordingSetIterator(files);

        if (iter == end(iter)) {
            // There are no events in the recording.
            LOGV2_INFO(10893009, "Empty or invalid recording - exiting");
            return;
        }

        // State for sessions will be constructed a little "earlier" than the time
        // at which the session needs to start, to avoid initial delays from e.g.,
        // spawning the thread.
        const auto timePadding = replayConfig.sessionPreInitTime;

        // Plan the replay to start a small time into the future, so sessions can be
        // constructed ready to replay at the "correct" time.
        const auto replayStartTime = std::chrono::steady_clock::now() + timePadding;

        // create a new session handler for managing the recording.
        SessionHandler sessionHandler{
            replayConfig.mongoURI, replayStartTime, replayConfig.enablePerformanceRecording};

        mongo::stop_callback sc(stop, [&] { sessionHandler.stopAllSessions(); });

        LOGV2_INFO(10893005, "Replay starting");


        for (; iter != end(iter); ++iter) {
            // Read ahead by a small time window to find session starts, to initialize session
            // state with a small grace period before the first event for that session needs to be
            // processed.
            // Reading too far (or unlimited) ahead would needlessly create session state before
            // it is needed, wasting resources.
            auto nextEventTS = replayStartTime + iter->offset.toSystemDuration() - timePadding;

            if (!sessionHandler.waitUntil(nextEventTS)) {
                // Didn't reach the expected time; a session failed or stop was requested by the
                // caller.
                break;
            }

            ReplayCommand command{*iter};
            if (command.isSessionStart()) {
                sessionHandler.createSession(command.fetchRequestSessionId(), iter);
            }
        }

        // All sessions seen in the recording have been created, and are independently replaying
        // in dedicated threads.
        LOGV2_INFO(10893006, "All sessions initialized");

        // Wait for all the sessions to complete.
        sessionHandler.waitForRunningSessions();

        sessionHandler.rethrowIfSessionFailed();

    } catch (DBException& e) {
        LOGV2_INFO(10893010, "Replay failed", "exception"_attr = e.what());
        e.addContext("Session replay failed");
        throw;
    } catch (const std::exception& e) {
        LOGV2_INFO(10893007, "Replay failed", "exception"_attr = e.what());
        throw;
    }
    LOGV2_INFO(10893008, "Replay completed");
}

void ReplayClient::replayRecording(const ReplayConfigs& configs) {
    ParallelExecutor::apply(configs, recordingDispatcher);
}

void ReplayClient::replayRecording(const std::string& recordingFileName, const std::string& uri) {
    ReplayConfig config{recordingFileName, uri};
    replayRecording({config});
}

}  // namespace mongo
