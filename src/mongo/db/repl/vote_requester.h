// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
