// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <queue>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

namespace mongo {

/**
 * Initialized by adding results to its results queue, it then passes through the results in its
 * queue until the queue is empty.
 */
class RouterStageQueuedData final : public RouterExecStage {
public:
    RouterStageQueuedData(OperationContext* opCtx) : RouterExecStage(opCtx) {}
    ~RouterStageQueuedData() final {}

    StatusWith<ClusterQueryResult> next() final;

    Status releaseMemory() final {
        // It has no children. It cannot do anything to release memory.
        return Status::OK();
    }

    void kill(OperationContext* opCtx) final;

    bool remotesExhausted() const final;

    bool isEOF() const final {
        return _resultsQueue.empty();
    }

    std::size_t getNumRemotes() const final;

    /**
     * Queues a BSONObj to be returned.
     */
    void queueResult(ClusterQueryResult&& result);

    /**
     * Queues an error response.
     */
    void queueError(Status status);

private:
    std::queue<StatusWith<ClusterQueryResult>> _resultsQueue;
};

}  // namespace mongo
