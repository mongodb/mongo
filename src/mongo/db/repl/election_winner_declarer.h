/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class Status;

namespace repl {

class ScatterGatherRunner;
class ReplSetDeclareElectionWinnerArgs;

class ElectionWinnerDeclarer {
    MONGO_DISALLOW_COPYING(ElectionWinnerDeclarer);

public:
    class Algorithm : public ScatterGatherAlgorithm {
    public:
        Algorithm(const std::string& setName,
                  long long winnerId,
                  long long term,
                  const std::vector<HostAndPort>& targets);
        virtual ~Algorithm();
        virtual std::vector<executor::RemoteCommandRequest> getRequests() const;
        virtual void processResponse(const executor::RemoteCommandRequest& request,
                                     const ResponseStatus& response);
        virtual bool hasReceivedSufficientResponses() const;

        /**
         * Returns a Status indicating what if anything went wrong while declaring the
         * election winner.
         *
         * It is invalid to call this before hasReceivedSufficeintResponses returns true.
         */
        Status getStatus() const {
            return _status;
        }

    private:
        const std::string _setName;
        const long long _winnerId;
        const long long _term;
        const std::vector<HostAndPort> _targets;
        bool _failed = false;
        long long _responsesProcessed = 0;
        Status _status = Status::OK();
    };

    ElectionWinnerDeclarer();
    virtual ~ElectionWinnerDeclarer();

    /**
     * Begins the process of sending replSetDeclareElectionWinner commands to all non-DOWN nodes
     * in currentConfig, with the intention of alerting them of a new primary.
     *
     * evh can be used to schedule a callback when the process is complete.
     * This function must be run in the executor, as it must be synchronous with the command
     * callbacks that it schedules.
     * If this function returns Status::OK(), evh is then guaranteed to be signaled.
     **/
    StatusWith<ReplicationExecutor::EventHandle> start(
        ReplicationExecutor* executor,
        const std::string& setName,
        long long winnerId,
        long long term,
        const std::vector<HostAndPort>& targets,
        const stdx::function<void()>& onCompletion = stdx::function<void()>());

    /**
     * Informs the ElectionWinnerDeclarer to cancel further processing.  The "executor"
     * argument must point to the same executor passed to "start()".
     *
     * Like start(), this method must run in the executor context.
     */
    void cancel(ReplicationExecutor* executor);

    /**
     * Returns a Status from the ElectionWinnerDeclarer::algorithm which indicates what
     * if anything went wrong while declaring the election winner.
     *
     * It is invalid to call this before the ElectionWinnerDeclarer::algorithm finishes running.
     */
    Status getStatus() const;

private:
    std::unique_ptr<Algorithm> _algorithm;
    std::unique_ptr<ScatterGatherRunner> _runner;
    bool _isCanceled = false;
};

}  // namespace repl
}  // namespace mongo
