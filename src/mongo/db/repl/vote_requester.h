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

#include <memory>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class Status;

namespace repl {

class ScatterGatherRunner;

class VoteRequester {
    MONGO_DISALLOW_COPYING(VoteRequester);

public:
    enum class Result {
        kSuccessfullyElected,
        kStaleTerm,
        kInsufficientVotes,
    };

    class Algorithm : public ScatterGatherAlgorithm {
    public:
        Algorithm(const ReplicaSetConfig& rsConfig,
                  long long candidateIndex,
                  long long term,
                  bool dryRun,
                  OpTime lastDurableOpTime);
        virtual ~Algorithm();
        virtual std::vector<executor::RemoteCommandRequest> getRequests() const;
        virtual void processResponse(const executor::RemoteCommandRequest& request,
                                     const ResponseStatus& response);
        virtual bool hasReceivedSufficientResponses() const;

        /**
         * Returns a VoteRequest::Result indicating the result of the election.
         *
         * It is invalid to call this before hasReceivedSufficientResponses returns true.
         */
        Result getResult() const;

        /**
         * Returns the list of nodes that responded to the VoteRequest command.
         */
        unordered_set<HostAndPort> getResponders() const;

    private:
        const ReplicaSetConfig _rsConfig;
        const long long _candidateIndex;
        const long long _term;
        bool _dryRun = false;  // this bool indicates this is a mock election when true
        const OpTime _lastDurableOpTime;
        std::vector<HostAndPort> _targets;
        unordered_set<HostAndPort> _responders;
        bool _staleTerm = false;
        long long _responsesProcessed = 0;
        long long _votes = 1;
    };

    VoteRequester();
    virtual ~VoteRequester();

    /**
     * Begins the process of sending replSetRequestVotes commands to all non-DOWN nodes
     * in currentConfig, in attempt to receive sufficient votes to win the election.
     *
     * evh can be used to schedule a callback when the process is complete.
     * If this function returns Status::OK(), evh is then guaranteed to be signaled.
     **/
    StatusWith<ReplicationExecutor::EventHandle> start(ReplicationExecutor* executor,
                                                       const ReplicaSetConfig& rsConfig,
                                                       long long candidateIndex,
                                                       long long term,
                                                       bool dryRun,
                                                       OpTime lastDurableOpTime);

    /**
     * Informs the VoteRequester to cancel further processing.
     */
    void cancel();

    Result getResult() const;
    unordered_set<HostAndPort> getResponders() const;

private:
    std::unique_ptr<Algorithm> _algorithm;
    std::unique_ptr<ScatterGatherRunner> _runner;
    bool _isCanceled = false;
};

}  // namespace repl
}  // namespace mongo
