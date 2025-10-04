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
