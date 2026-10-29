// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

namespace repl {

/**
 * Interface for a specialization of a scatter-gather algorithm that sends
 * requests to a set of targets, and then processes responses until it has
 * seen enough.
 *
 * To use, call getRequests() to get a vector of request objects describing network operations.
 * Start performing the network operations in any order, and then, until
 * hasReceivedSufficientResponses() returns true, call processResponse for each response as it
 * arrives.  Once hasReceivedSufficientResponses() you may cancel outstanding network
 * operations, and must stop calling processResponse.  Implementations of this interface may
 * assume that processResponse() is never called after hasReceivedSufficientResponses() returns
 * true.
 */
class ScatterGatherAlgorithm {
public:
    /**
     * Returns the list of requests that should be sent.
     */
    virtual std::vector<executor::RemoteCommandRequest> getRequests() const = 0;

    /**
     * Method to call once for each received response.
     */
    virtual void processResponse(const executor::RemoteCommandRequest& request,
                                 const executor::RemoteCommandResponse& response) = 0;

    /**
     * Returns true if no more calls to processResponse are needed to consider the
     * algorithm complete.  Once this method returns true, one should no longer
     * call processResponse.
     */
    virtual bool hasReceivedSufficientResponses() const = 0;

protected:
    virtual ~ScatterGatherAlgorithm();  // Shouldn't actually be virtual.
};

}  // namespace repl
}  // namespace mongo
