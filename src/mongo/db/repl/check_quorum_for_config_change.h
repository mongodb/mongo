// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <utility>
#include <vector>

namespace mongo {
namespace repl {

class ReplSetConfig;

/**
 * Quorum checking state machine.
 *
 * Usage: Construct a QuorumChecker, pass in a pointer to the configuration for which you're
 * checking quorum, and the integer index of the member config representing the "executing"
 * node.  Use ScatterGatherRunner or otherwise execute a scatter-gather procedure as described
 * in the class comment for the ScatterGatherAlgorithm class.  After
 * hasReceivedSufficientResponses() returns true, you may call getFinalStatus() to get the
 * result of the quorum check.
 */
class QuorumChecker : public ScatterGatherAlgorithm {
    QuorumChecker(const QuorumChecker&) = delete;
    QuorumChecker& operator=(const QuorumChecker&) = delete;

public:
    /**
     * Constructs a QuorumChecker that is used to confirm that sufficient nodes are up to accept
     * "rsConfig".  "myIndex" is the index of the local node, which is assumed to be up.
     *
     * "rsConfig" must stay in scope until QuorumChecker's destructor completes.
     */
    QuorumChecker(const ReplSetConfig* rsConfig, int myIndex, long long term);
    ~QuorumChecker() override;

    std::vector<executor::RemoteCommandRequest> getRequests() const override;
    void processResponse(const executor::RemoteCommandRequest& request,
                         const executor::RemoteCommandResponse& response) override;

    bool hasReceivedSufficientResponses() const override;

    Status getFinalStatus() const {
        return _finalStatus;
    }

private:
    /**
     * Callback that executes after _haveReceivedSufficientReplies() becomes true.
     *
     * Computes the quorum result based on responses received so far, stores it into
     * _finalStatus, and enables QuorumChecker::run() to return.
     */
    void _onQuorumCheckComplete();

    /**
     * Updates the QuorumChecker state based on the data from a single heartbeat response.
     */
    void _tabulateHeartbeatResponse(const executor::RemoteCommandRequest& request,
                                    const executor::RemoteCommandResponse& response);

    /**
     * Adds information about each failed heartbeat response to the provided stream with the format:
     * "<host:port> failed with <errmsg>, <host:port> failed with <errmsg>".
     */
    void _appendFailedHeartbeatResponses(str::stream& stream);

    /**
     * Adds information about each fully successful voting node to the provided stream with the
     * format:
     * "<host:port>, <host:port>"
     * A fully successful voting node is one which replied success over its main port and either
     * does not have a priority port configured or also replied success over the priority port.
     */
    void _appendFullySuccessfulVotingHostAndPorts(str::stream& stream, int expectedResponses);

    // Pointer to the replica set configuration for which we're checking quorum.
    const ReplSetConfig* const _rsConfig;

    // Index of the local node's member configuration in _rsConfig.
    const int _myIndex;

    // The term of this node.
    const long long _term;

    struct ResponseStatus {
        bool mainResponseReceived;
        bool priorityResponseReceived;
        bool fullySuccessful;
    };
    // Tracks main and priority port responses for each member. The indexes into this vector will
    // be the same as that of the _rsConfig and entries for non-voters will be all false.
    std::vector<ResponseStatus> _responses;
    // Tracks the number of voters for which their state is fully successful (meaning they have
    // responded on the main port and do not have a priority port configured or have responded
    // on both main and priority ports).
    int _successfulVoterCount;

    // List of nodes with bad responses and the bad response status they returned.
    std::vector<std::pair<HostAndPort, Status>> _badResponses;

    // Total number of responses and timeouts processed.
    int _numResponses;

    // Number of electable nodes that have responded affirmatively (on both their main and
    // priority ports).
    int _numElectable;

    // Number of responses required. This will be equal to the number of members in the config plus
    // the number of members that have priority ports specified since we need to contact both
    // ports in that case.
    int _numResponsesRequired;

    // Set to a non-OK status if a response from a remote node indicates
    // that the quorum check should definitely fail, such as because of
    // a replica set name mismatch.
    Status _vetoStatus;

    // Final status of the quorum check, returned by run().
    Status _finalStatus;
};

/**
 * Performs a quorum call to determine if a sufficient number of nodes are up
 * to initiate a replica set with configuration "rsConfig".
 *
 * "myIndex" is the index of this node's member configuration in "rsConfig".
 * "executor" is the event loop in which to schedule network/aysnchronous processing.
 * "term" is the term of this node.
 *
 * For purposes of initiate, a quorum is only met if all of the following conditions
 * are met:
 * - All nodes respond.
 * - No nodes other than the node running the quorum check have data.
 * - No nodes are already joined to a replica set.
 * - No node reports a replica set name other than the one in "rsConfig".
 */
Status checkQuorumForInitiate(executor::TaskExecutor* executor,
                              const ReplSetConfig& rsConfig,
                              int myIndex,
                              long long term);

/**
 * Performs a quorum call to determine if a sufficient number of nodes are up
 * to replace the current replica set configuration with "rsConfig".
 *
 * "myIndex" is the index of this node's member configuration in "rsConfig".
 * "executor" is the event loop in which to schedule network/aysnchronous processing.
 * "term" is the term of this node.
 *
 * For purposes of reconfig, a quorum is only met if all of the following conditions
 * are met:
 * - A majority of voting nodes respond.
 * - At least one electable node responds.
 * - No responding node reports a replica set name other than the one in "rsConfig".
 * - All responding nodes report a config version less than the one in "rsConfig".
 */
Status checkQuorumForReconfig(executor::TaskExecutor* executor,
                              const ReplSetConfig& rsConfig,
                              int myIndex,
                              long long term);

}  // namespace repl
}  // namespace mongo
