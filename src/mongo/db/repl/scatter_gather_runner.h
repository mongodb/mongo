/**
 *    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

template <typename T>
class StatusWith;

namespace repl {

class ScatterGatherAlgorithm;

/**
 * Interface of a scatter-gather behavior.
 */
class ScatterGatherRunner {
    MONGO_DISALLOW_COPYING(ScatterGatherRunner);

public:
    /**
     * Constructs a new runner whose underlying algorithm is "algorithm".
     *
     * "algorithm" and "executor" must remain in scope until the runner's destructor completes.
     */
    explicit ScatterGatherRunner(ScatterGatherAlgorithm* algorithm, ReplicationExecutor* executor);

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
    StatusWith<ReplicationExecutor::EventHandle> start();

    /**
     * Informs the runner to cancel further processing.
     */
    void cancel();

private:
    /**
     * Implementation of a scatter-gather behavior using a ReplicationExecutor.
     */
    class RunnerImpl {
    public:
        explicit RunnerImpl(ScatterGatherAlgorithm* algorithm, ReplicationExecutor* executor);

        /**
         * On success, returns an event handle that will be signaled when the runner has
         * finished executing the scatter-gather process.  After that event has been
         * signaled, it is safe for the caller to examine any state on "algorithm".
         *
         * The returned event will eventually be signaled.
         */
        StatusWith<ReplicationExecutor::EventHandle> start(
            const ReplicationExecutor::RemoteCommandCallbackFn cb);

        /**
         * Informs the runner to cancel further processing.
         */
        void cancel();

        /**
         * Callback invoked once for every response from the network.
         */
        void processResponse(const ReplicationExecutor::RemoteCommandCallbackArgs& cbData);

    private:
        /**
         * Method that performs all actions required when _algorithm indicates a sufficient
         * number of responses have been received.
         */
        void _signalSufficientResponsesReceived();

        ReplicationExecutor* _executor;      // Not owned here.
        ScatterGatherAlgorithm* _algorithm;  // Not owned here.
        ReplicationExecutor::EventHandle _sufficientResponsesReceived;
        std::vector<ReplicationExecutor::CallbackHandle> _callbacks;
        bool _started = false;
        stdx::mutex _mutex;
    };

    ReplicationExecutor* _executor;  // Not owned here.

    // This pointer of RunnerImpl will be shared with remote command callbacks to make sure
    // callbacks can access the members safely.
    std::shared_ptr<RunnerImpl> _impl;
};

}  // namespace repl
}  // namespace mongo
