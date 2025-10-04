/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class Status;

namespace repl {

class VoteRequester {
    VoteRequester(const VoteRequester&) = delete;
    VoteRequester& operator=(const VoteRequester&) = delete;

public:
    enum class Result {
        kSuccessfullyElected,
        kStaleTerm,
        kInsufficientVotes,
        kPrimaryRespondedNo,
        kCancelled
    };

    class Algorithm : public ScatterGatherAlgorithm {
    public:
        Algorithm(const ReplSetConfig& rsConfig,
                  long long candidateIndex,
                  long long term,
                  bool dryRun,
                  OpTime lastWrittenOpTime,
                  OpTime lastAppliedOpTime,
                  int primaryIndex);
        ~Algorithm() override;
        std::vector<executor::RemoteCommandRequest> getRequests() const override;
        void processResponse(const executor::RemoteCommandRequest& request,
                             const executor::RemoteCommandResponse& response) override;
        bool hasReceivedSufficientResponses() const override;

        /**
         * Returns a VoteRequest::Result indicating the result of the election.
         *
         * It is invalid to call this before hasReceivedSufficientResponses returns true.
         */
        Result getResult() const;

        /**
         * Returns the list of nodes that responded to the VoteRequest command.
         */
        stdx::unordered_set<HostAndPort> getResponders() const;

    private:
        enum class PrimaryVote { Pending, Yes, No };

        const ReplSetConfig _rsConfig;
        const long long _candidateIndex;
        const long long _term;
        bool _dryRun = false;  // this bool indicates this is a mock election when true
        const OpTime _lastWrittenOpTime;
        const OpTime _lastAppliedOpTime;
        std::vector<HostAndPort> _targets;
        stdx::unordered_set<HostAndPort> _responders;
        bool _staleTerm = false;
        long long _responsesProcessed = 0;
        long long _votes = 1;
        boost::optional<HostAndPort> _primaryHost;
        PrimaryVote _primaryVote = PrimaryVote::Pending;
    };

    VoteRequester();
    virtual ~VoteRequester();

    /**
     * Begins the process of sending replSetRequestVotes commands to all non-DOWN nodes
     * in currentConfig, in attempt to receive sufficient votes to win the election.
     * If primaryIndex is not -1, then it means that the primary's vote is required
     * to win the elction.
     *
     * evh can be used to schedule a callback when the process is complete.
     * If this function returns Status::OK(), evh is then guaranteed to be signaled.
     **/
    StatusWith<executor::TaskExecutor::EventHandle> start(executor::TaskExecutor* executor,
                                                          const ReplSetConfig& rsConfig,
                                                          long long candidateIndex,
                                                          long long term,
                                                          bool dryRun,
                                                          OpTime lastWrittenOpTime,
                                                          OpTime lastAppliedOpTime,
                                                          int primaryIndex);

    /**
     * Informs the VoteRequester to cancel further processing.
     */
    void cancel();

    Result getResult() const;
    stdx::unordered_set<HostAndPort> getResponders() const;

private:
    std::shared_ptr<Algorithm> _algorithm;
    std::unique_ptr<ScatterGatherRunner> _runner;
};

}  // namespace repl
}  // namespace mongo
