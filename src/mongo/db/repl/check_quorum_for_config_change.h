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

    // Pointer to the replica set configuration for which we're checking quorum.
    const ReplSetConfig* const _rsConfig;

    // Index of the local node's member configuration in _rsConfig.
    const int _myIndex;

    // The term of this node.
    const long long _term;

    // List of voting nodes that have responded affirmatively.
    std::vector<HostAndPort> _voters;

    // List of nodes with bad responses and the bad response status they returned.
    std::vector<std::pair<HostAndPort, Status>> _badResponses;

    // Total number of responses and timeouts processed.
    int _numResponses;

    // Number of electable nodes that have responded affirmatively.
    int _numElectable;

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
