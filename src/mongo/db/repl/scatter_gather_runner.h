// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mongo {

template <typename T>
class StatusWith;

namespace repl {

class ScatterGatherAlgorithm;

/**
 * Interface of a scatter-gather behavior.
 */
class ScatterGatherRunner {
    ScatterGatherRunner(const ScatterGatherRunner&) = delete;
    ScatterGatherRunner& operator=(const ScatterGatherRunner&) = delete;

public:
    /**
     * Constructs a new runner whose underlying algorithm is "algorithm".
     *
     * "executor" must remain in scope until the runner's destructor completes.
     * "algorithm" is shared between the runner and the caller.
     * "logMessage" is the process for which this ScatterGatherRunner is used. It will be included
     * in log lines written by the ScatterGatherRunner for remote command requests.
     */
    explicit ScatterGatherRunner(std::shared_ptr<ScatterGatherAlgorithm> algorithm,
                                 executor::TaskExecutor* executor,
                                 std::string logMessage);

    /**
     * Runs the scatter-gather process and blocks until it completes.
     *
     * Must _not_ be run from inside the executor context.
     *
     * Returns ErrorCodes::ShutdownInProgress if the executor enters or is already in
     * the shutdown state before run() can schedule execution of the scatter-gather
     * in the executor.  Note that if the executor is shut down after the algorithm
     * is scheduled but before it completes, this method will return Status::OK(),
     * just as it does when it runs successfully to completion.
     */
    Status run();

    /**
     * On success, returns an event handle that will be signaled when the runner has
     * finished executing the scatter-gather process.  After that event has been
     * signaled, it is safe for the caller to examine any state on "algorithm".
     *
     * The returned event will eventually be signaled.
     */
    StatusWith<executor::TaskExecutor::EventHandle> start();

    /**
     * Informs the runner to cancel further processing.
     */
    void cancel();

private:
    /**
     * Implementation of a scatter-gather behavior using a TaskExecutor.
     */
    class RunnerImpl {
    public:
        explicit RunnerImpl(std::shared_ptr<ScatterGatherAlgorithm> algorithm,
                            executor::TaskExecutor* executor,
                            std::string logMessage);

        /**
         * On success, returns an event handle that will be signaled when the runner has
         * finished executing the scatter-gather process.  After that event has been
         * signaled, it is safe for the caller to examine any state on "algorithm".
         *
         * The returned event will eventually be signaled.
         */
        StatusWith<executor::TaskExecutor::EventHandle> start(
            executor::TaskExecutor::RemoteCommandCallbackFn cb);

        /**
         * Informs the runner to cancel further processing.
         */
        void cancel();

        /**
         * Callback invoked once for every response from the network.
         */
        void processResponse(const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData);

    private:
        /**
         * Method that performs all actions required when _algorithm indicates a sufficient
         * number of responses have been received.
         */
        void _signalSufficientResponsesReceived();

        executor::TaskExecutor* _executor;  // Not owned here.
        std::shared_ptr<ScatterGatherAlgorithm> _algorithm;
        std::string _logMessage;
        executor::TaskExecutor::EventHandle _sufficientResponsesReceived;
        std::vector<executor::TaskExecutor::CallbackHandle> _callbacks;
        bool _started = false;
        std::mutex _mutex;
    };

    executor::TaskExecutor* _executor;  // Not owned here.

    // This pointer of RunnerImpl will be shared with remote command callbacks to make sure
    // callbacks can access the members safely.
    std::shared_ptr<RunnerImpl> _impl;
};

}  // namespace repl
}  // namespace mongo
