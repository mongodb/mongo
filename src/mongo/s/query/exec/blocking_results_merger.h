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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/exec/async_results_merger.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/next_high_watermark_determining_strategy.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/s/query/exec/shard_tag.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace mongo {

/**
 * Layers a simpler blocking interface on top of the AsyncResultsMerger from which this
 * BlockingResultsMerger is constructed.
 */
class BlockingResultsMerger {
public:
    BlockingResultsMerger(OperationContext* opCtx,
                          AsyncResultsMergerParams&& arm,
                          std::shared_ptr<executor::TaskExecutor> executor,
                          std::unique_ptr<ResourceYielder> resourceYielder);

    ~BlockingResultsMerger();

    /**
     * Returns a const reference to the AsyncResultsMergerParams owned by the AsyncResultsMerger.
     */
    const AsyncResultsMergerParams& asyncResultsMergerParams() const;

    /**
     * Blocks until the next result is available or an error is detected.
     */
    StatusWith<ClusterQueryResult> next(OperationContext*);

    Status releaseMemory();

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
        return _arm->setAwaitDataTimeout(awaitDataTimeout);
    }

    void reattachToOperationContext(OperationContext* opCtx) {
        _arm->reattachToOperationContext(opCtx);
    }

    void detachFromOperationContext() {
        _arm->detachFromOperationContext();
    }

    bool remotesExhausted() const {
        return _arm->remotesExhausted();
    }

    bool partialResultsReturned() const {
        return _arm->partialResultsReturned();
    }

    std::size_t getNumRemotes() const {
        return _arm->getNumRemotes();
    }

    /**
     * Enables buffering of the last returned result in the underlying 'AsyncResultsMerger'. This is
     * used by v2 change stream readers.
     */
    void enableUndoNextMode() {
        _arm->enableUndoNextReadyMode();
    }

    /**
     * Disables buffering of the last returned result in the underlying 'AsyncResultsMerger'. This
     * is used by v2 change stream readers.
     */
    void disableUndoNextMode() {
        _arm->disableUndoNextReadyMode();
    }

    /**
     * Undoes the effect of fetching the last returned result via 'next()' from the underlying
     * 'AsyncResultsMerger'. For a more detailed description refer to the code comments for
     * 'AsyncResultsMerger::undoNextReady()'. This is used by v2 change stream readers.
     */
    void undoNext() {
        _arm->undoNextReady();
    }

    /**
     * Makes the underlying 'AsyncResultsMerger' recognize change stream control events. This is
     * used by v2 change stream readers.
     */
    void recognizeControlEvents();

    /**
     * Returns the current high water mark from the underlying 'AsyncResultsMerger'.
     */
    BSONObj getHighWaterMark() {
        return _arm->getHighWaterMark();
    }

    /**
     * Sets the initial high watermark to return when no cursors are tracked.
     */
    void setInitialHighWaterMark(const BSONObj& highWaterMark);

    /**
     * Sets the current high water mark of the underlying 'AsyncResultsMerger'. Notably this allows
     * to set the high water mark to a timestamp earlier than the current high water mark.
     */
    void setHighWaterMark(const BSONObj& highWaterMark) {
        _arm->setHighWaterMark(highWaterMark);
    }

    void addNewShardCursors(std::vector<RemoteCursor>&& newCursors,
                            const ShardTag& tag = ShardTag::kDefault) {
        _arm->addNewShardCursors(std::move(newCursors), tag);
    }

    /**
     * Closes and removes all cursors belonging to any of the specified shardIds. All in-flight
     * requests to any of these remote cursors will be canceled and discarded.
     * All results from the to-be closed remotes that have already been received but have not been
     * consumed will be kept. They can be consumed normally.
     * Closing remote cursors is only supported for tailable, awaitData cursors.
     */
    void closeShardCursors(const stdx::unordered_set<ShardId>& shardIds, const ShardTag& tag) {
        _arm->closeShardCursors(shardIds, tag);
    }

    /**
     * Sets the strategy to determine the next high water mark.
     * Assumes that the 'AsyncResultsMerger' is in tailable, awaitData mode.
     */
    void setNextHighWaterMarkDeterminingStrategy(
        NextHighWaterMarkDeterminingStrategyPtr nextHighWaterMarkDeterminingStrategy) {
        _arm->setNextHighWaterMarkDeterminingStrategy(
            std::move(nextHighWaterMarkDeterminingStrategy));
    }

    /**
     * Blocks until '_arm' has been killed, which involves cleaning up any remote cursors managed
     * by this results merger.
     */
    void kill(OperationContext* opCtx);

    query_stats::DataBearingNodeMetrics takeMetrics() {
        return _arm->takeMetrics();
    }

private:
    /**
     * Awaits the next result from the ARM with no time limit.
     */
    StatusWith<ClusterQueryResult> blockUntilNext(OperationContext* opCtx);

    /**
     * Awaits the next result from the ARM up to the time limit specified on 'opCtx'. If this is the
     * user's initial find or we have already obtained at least one result for this batch, this
     * method returns EOF immediately rather than blocking.
     */
    StatusWith<ClusterQueryResult> awaitNextWithTimeout(OperationContext* opCtx);

    /**
     * Returns the next event to wait upon - either a new event from the ARM, or a valid preceding
     * event which we scheduled during the previous call to next().
     */
    StatusWith<executor::TaskExecutor::EventHandle> getNextEvent();

    /**
     * Calls the waitFn and return the result, yielding resources while waiting if necessary.
     * 'waitFn' may not throw.
     */
    StatusWith<stdx::cv_status> doWaiting(
        OperationContext* opCtx,
        const std::function<StatusWith<stdx::cv_status>()>& waitFn) noexcept;

    TailableModeEnum _tailableMode;
    std::shared_ptr<executor::TaskExecutor> _executor;

    // In a case where we have a tailable, awaitData cursor, a call to 'next()' will block waiting
    // for an event generated by '_arm', but may time out waiting for this event to be triggered.
    // While it's waiting, the time limit for the 'awaitData' piece of the cursor may have been
    // exceeded. When this happens, we use '_leftoverEventFromLastTimeout' to remember the old event
    // and pick back up waiting for it on the next call to 'next()'.
    executor::TaskExecutor::EventHandle _leftoverEventFromLastTimeout;

    // The 'AsyncResultsMerger' is fully owned by this 'BlockingResultsMerger', but we need a
    // shared_ptr to keep the ARM alive and valid until all of its async requests have been
    // processed successfully.
    std::shared_ptr<AsyncResultsMerger> _arm;

    // Provides interface for yielding and "unyielding" resources while waiting for results from
    // the network. A value of nullptr implies that no such yielding or unyielding is necessary.
    std::unique_ptr<ResourceYielder> _resourceYielder;
};

}  // namespace mongo
