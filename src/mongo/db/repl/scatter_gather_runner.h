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

namespace mongo {

    template <typename T> class StatusWith;

namespace repl {

    class ScatterGatherAlgorithm;

    /**
     * Implementation of a scatter-gather behavior using a ReplicationExecutor.
     */
    class ScatterGatherRunner {
        MONGO_DISALLOW_COPYING(ScatterGatherRunner);
    public:
        /**
         * Constructs a new runner whose underlying algorithm is "algorithm".
         *
         * "algorithm" must remain in scope until the runner's destructor completes.
         */
        explicit ScatterGatherRunner(ScatterGatherAlgorithm* algorithm);

        ~ScatterGatherRunner();

        /**
         * Runs the scatter-gather process using "executor", and blocks until it completes.
         *
         * Must _not_ be run from inside the executor context.
         *
         * Returns ErrorCodes::ShutdownInProgress if the executor enters or is already in
         * the shutdown state before run() can schedule execution of the scatter-gather
         * in the executor.  Note that if the executor is shut down after the algorithm
         * is scheduled but before it completes, this method will return Status::OK(),
         * just as it does when it runs successfully to completion.
         */
        Status run(ReplicationExecutor* executor);

        /**
         * Starts executing the scatter-gather process using "executor".
         *
         * On success, returns an event handle that will be signaled when the runner has
         * finished executing the scatter-gather process.  After that event has been
         * signaled, it is safe for the caller to examine any state on "algorithm".
         *
         * This method must be called inside the executor context.
         *
         * onCompletion is an optional callback that will be executed in executor context
         * immediately prior to signaling the event handle returned here. It must never
         * throw exceptions.  It may examine the state of the algorithm object.
         *
         * NOTE: If the executor starts to shut down before onCompletion executes, onCompletion may
         * never execute, even though the returned event will eventually be signaled.
         */
        StatusWith<ReplicationExecutor::EventHandle> start(
                ReplicationExecutor* executor,
                const stdx::function<void ()>& onCompletion = stdx::function<void ()>());

    private:
        /**
         * Callback invoked once for every response from the network.
         */
        static void _processResponse(const ReplicationExecutor::RemoteCommandCallbackData& cbData,
                                     ScatterGatherRunner* runner);

        /**
         * Method that performs all actions required when _algorithm indicates a sufficient
         * number of respones have been received.
         */
        void _signalSufficientResponsesReceived(ReplicationExecutor* executor);

        ScatterGatherAlgorithm* _algorithm;
        stdx::function<void ()> _onCompletion;
        ReplicationExecutor::EventHandle _sufficientResponsesReceived;
        std::vector<ReplicationExecutor::CallbackHandle> _callbacks;
        size_t _actualResponses;
        bool _started;
    };

}  // namespace repl
}  // namespace mongo
