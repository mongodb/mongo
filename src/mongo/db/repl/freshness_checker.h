/**
 *    Copyright (C) 2014 MongoDB Inc.
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
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"

namespace mongo {

class Status;

namespace repl {

class ReplicaSetConfig;
class ScatterGatherRunner;

class FreshnessChecker {
    MONGO_DISALLOW_COPYING(FreshnessChecker);

public:
    enum ElectionAbortReason {
        None = 0,
        FresherNodeFound,  // Freshness check found fresher node
        FreshnessTie,  // Freshness check resulted in one or more nodes with our lastAppliedOpTime
        QuorumUnavailable,  // Not enough up voters
        QuorumUnreachable   // Too many failed voter responses
    };

    class Algorithm : public ScatterGatherAlgorithm {
    public:
        Algorithm(Timestamp lastOpTimeApplied,
                  const ReplicaSetConfig& rsConfig,
                  int selfIndex,
                  const std::vector<HostAndPort>& targets);
        virtual ~Algorithm();
        virtual std::vector<executor::RemoteCommandRequest> getRequests() const;
        virtual void processResponse(const executor::RemoteCommandRequest& request,
                                     const ResponseStatus& response);
        virtual bool hasReceivedSufficientResponses() const;
        ElectionAbortReason shouldAbortElection() const;

    private:
        // Returns true if the number of failed votes is over _losableVotes()
        bool hadTooManyFailedVoterResponses() const;

        // Returns true if the member, by host and port, has a vote.
        bool _isVotingMember(const HostAndPort host) const;

        // Number of responses received so far.
        int _responsesProcessed;

        // Number of failed voter responses so far.
        int _failedVoterResponses;

        // Last Timestamp applied by the caller; used in the Fresh command
        const Timestamp _lastOpTimeApplied;

        // Config to use for this check
        const ReplicaSetConfig _rsConfig;

        // Our index position in _rsConfig
        const int _selfIndex;

        // The UP members we are checking
        const std::vector<HostAndPort> _targets;

        // Number of voting targets
        int _votingTargets;

        // Number of voting nodes which can error
        int _losableVoters;

        // 1 if I have a vote, otherwise 0
        int _myVote;

        // Reason to abort, start with None
        ElectionAbortReason _abortReason;
    };

    FreshnessChecker();
    virtual ~FreshnessChecker();

    /**
     * Begins the process of sending replSetFresh commands to all non-DOWN nodes
     * in currentConfig, with the intention of determining whether the current node
     * is freshest.
     * evh can be used to schedule a callback when the process is complete.
     * If this function returns Status::OK(), evh is then guaranteed to be signaled.
     **/
    StatusWith<ReplicationExecutor::EventHandle> start(ReplicationExecutor* executor,
                                                       const Timestamp& lastOpTimeApplied,
                                                       const ReplicaSetConfig& currentConfig,
                                                       int selfIndex,
                                                       const std::vector<HostAndPort>& targets);

    /**
     * Informs the freshness checker to cancel further processing.
     */
    void cancel();

    /**
     * Returns true if cancel() was called on this instance.
     */
    bool isCanceled() const {
        return _isCanceled;
    }

    /**
     * 'None' if the election should continue, otherwise the reason to abort
     */
    ElectionAbortReason shouldAbortElection() const;

    /**
     * Returns the config version supplied in the config when start() was called.
     * Useful for determining if the the config version has changed.
     */
    long long getOriginalConfigVersion() const;

private:
    std::unique_ptr<Algorithm> _algorithm;
    std::unique_ptr<ScatterGatherRunner> _runner;
    long long _originalConfigVersion;
    bool _isCanceled;
};

}  // namespace repl
}  // namespace mongo
