// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <queue>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Initialized by adding results to its results queue, it then passes through the results in its
 * queue until the queue is empty.
 */
class RouterStageMock final : public RouterExecStage {
public:
    RouterStageMock(OperationContext* opCtx) : RouterExecStage(opCtx) {}
    ~RouterStageMock() final {}

    StatusWith<ClusterQueryResult> next() final;

    void kill(OperationContext* opCtx) final;

    bool remotesExhausted() const final;

    bool isEOF() const final;

    /**
     * Queues a BSONObj to be returned.
     */
    void queueResult(ClusterQueryResult&& result);

    /**
     * Queues an error response.
     */
    void queueError(Status status);

    /**
     * Queues an explicit boost::none response. The mock stage will also return boost::none
     * automatically after emptying the queue of responses.
     */
    void queueEOF();

    /**
     * Explicitly marks the remote cursors as all exhausted.
     */
    void markRemotesExhausted();

    /**
     * Gets the timeout for awaitData, or an error if none was set.
     */
    StatusWith<Milliseconds> getAwaitDataTimeout();

protected:
    Status doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

private:
    std::queue<StatusWith<ClusterQueryResult>> _resultsQueue;
    bool _remotesExhausted = false;
    boost::optional<Milliseconds> _awaitDataTimeout;
};

}  // namespace mongo
