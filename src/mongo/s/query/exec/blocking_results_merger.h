// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/shard_role/resource_yielder.h"
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

    bool isEOF() const {
        return _arm->isEOF();
    }

    bool partialResultsReturned() const {
        return _arm->partialResultsReturned();
    }

    std::size_t getNumRemotes() const {
        return _arm->getNumRemotes();
    }

    /**
     * Enables buffering of the last returned result in the underlying 'AsyncResultsMerger'. This is
     * used by v2 change stream readers in ignoreRemovedShardsMode.
     */
    void enableUndoNextMode() {
        _arm->enableUndoNextReadyMode();
    }

    /**
     * Disables buffering of the last returned result in the underlying 'AsyncResultsMerger'. This
     * is used by v2 change stream readers in ignoreRemovedShardsMode.
     */
    void disableUndoNextMode() {
        _arm->disableUndoNextReadyMode();
    }

    /**
     * Undoes the effect of fetching the last returned result via 'next()' from the underlying
     * 'AsyncResultsMerger'. This is used by v2 change stream readers in ignoreRemovedShards mode.
     * For a more detailed description refer to the code comments for
     * 'AsyncResultsMerger::undoNextReady()'.
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
     * Sets the current high water mark of the underlying 'AsyncResultsMerger'. Notably this allows
     * to set the high water mark to a timestamp earlier than the current high water mark.
     */
    void setHighWaterMark(const BSONObj& highWaterMark) {
        _arm->setHighWaterMark(highWaterMark);
    }

    /**
     * Adds the already opened cursors and their potential results to the underlying
     * 'AsyncResultsMerger'.
     */
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

    /**
     * Returns the next event to wait upon. This makes 'getNextEvent()', which is private,
     * accessible from within unit tests.
     */
    StatusWith<executor::TaskExecutor::EventHandle> getNextEvent_forTest() {
        return getNextEvent();
    }

private:
    /**
     * Awaits the next result from the AsyncResultsMerger with no time limit.
     */
    StatusWith<ClusterQueryResult> blockUntilNext(OperationContext* opCtx);

    /**
     * Awaits the next result from the AsyncResultsMerger up to the time limit specified on 'opCtx'.
     * If this is the user's initial find or we have already obtained at least one result for this
     * batch, this method returns EOF immediately rather than blocking.
     */
    StatusWith<ClusterQueryResult> awaitNextWithTimeout(OperationContext* opCtx);

    /**
     * Returns the next event to wait upon - either a new event from the AsyncResultsMerger, or a
     * valid preceding event which we scheduled during the previous call to next().
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
    // shared_ptr to keep the AsyncResultsMerger alive and valid until all of its async requests
    // have been processed successfully.
    std::shared_ptr<AsyncResultsMerger> _arm;

    // Provides interface for yielding and "unyielding" resources while waiting for results from
    // the network. A value of nullptr implies that no such yielding or unyielding is necessary.
    std::unique_ptr<ResourceYielder> _resourceYielder;
};

}  // namespace mongo
