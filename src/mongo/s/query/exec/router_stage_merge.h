// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/exec/blocking_results_merger.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>

namespace mongo {

/**
 * Serves as an adapter between the RouterExecStage interface and the BlockingResultsMerger
 * interface, providing a single stream of results populated from many remote streams.
 */
class RouterStageMerge final : public RouterExecStage {
public:
    RouterStageMerge(OperationContext* opCtx,
                     std::shared_ptr<executor::TaskExecutor> executor,
                     AsyncResultsMergerParams&& armParams)
        : RouterExecStage(opCtx),
          _resultsMerger(opCtx,
                         std::move(armParams),
                         std::move(executor),
                         TransactionRouterResourceYielder::makeForRemoteCommand()) {}

    StatusWith<ClusterQueryResult> next() final {
        return _resultsMerger.next(getOpCtx());
    }

    Status releaseMemory() final {
        auto res = _resultsMerger.releaseMemory();
        return res;
    }

    void kill(OperationContext* opCtx) final {
        _resultsMerger.kill(opCtx);
    }

    bool remotesExhausted() const final {
        return _resultsMerger.remotesExhausted();
    }

    bool isEOF() const final {
        return _resultsMerger.isEOF();
    }

    bool partialResultsReturned() const final {
        return _resultsMerger.partialResultsReturned();
    }

    std::size_t getNumRemotes() const final {
        return _resultsMerger.getNumRemotes();
    }

    BSONObj getPostBatchResumeToken() final {
        return _resultsMerger.getHighWaterMark();
    }

    boost::optional<query_stats::DataBearingNodeMetrics> takeRemoteMetrics() final {
        return _resultsMerger.takeMetrics();
    }

protected:
    Status doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) final {
        return _resultsMerger.setAwaitDataTimeout(awaitDataTimeout);
    }

    void doReattachToOperationContext() override {
        _resultsMerger.reattachToOperationContext(getOpCtx());
    }

    void doDetachFromOperationContext() override {
        _resultsMerger.detachFromOperationContext();
    }

private:
    // Schedules remote work and merges results from 'remotes'.
    BlockingResultsMerger _resultsMerger;
};

}  // namespace mongo
