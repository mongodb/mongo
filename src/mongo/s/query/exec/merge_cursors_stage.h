// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/s/query/exec/blocking_results_merger.h"
#include "mongo/s/query/exec/shard_tag.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace exec {
namespace agg {

class MergeCursorsStage final : public Stage {
public:
    MergeCursorsStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      const std::shared_ptr<BlockingResultsMerger>& blockingResultsMerger);

    bool usedDisk() const final;
    const SpecificStats* getSpecificStats() const final;
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext*) final;

    boost::optional<query_stats::DataBearingNodeMetrics> takeRemoteMetrics();
    std::size_t getNumRemotes() const;
    /**
     * Returns the high water mark sort key for the given cursor, if it exists; otherwise, returns
     * an empty BSONObj. Calling this method causes the underlying BlockingResultsMerger to be
     * populated and assumes ownership of the remote cursors.
     */
    BSONObj getHighWaterMark();
    bool remotesExhausted() const;
    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout);

    const PlanSummaryStats& getPlanSummaryStats() const {
        return _stats.planSummaryStats;
    }

    /**
     * Enables buffering of the last returned result in the underlying results merger. This is used
     * by v2 change stream readers in ignoreRemovedShards mode.
     */
    void enableUndoNextMode();

    /**
     * Disables buffering of the last returned result in the underlying results merger. This is used
     * by v2 change stream readers in ignoreRemovedShards mode.
     */
    void disableUndoNextMode();

    /**
     * Undoes the effect of fetching the last returned result via 'next()' from the underlying
     * results merger.
     * This method is used by v2 change stream readers in ignoreRemovedShards mode.
     */
    void undoNext();

    /**
     * Adds the specified, already opened cursors for remote shards or the config server.
     */
    void addNewShardCursors(std::vector<RemoteCursor>&& newCursors,
                            const ShardTag& tag = ShardTag::kDefault);

    /**
     * Closes the set of specified open cursors on remote shards and/or the config server.
     * This is used by v2 change stream readers.
     */
    void closeShardCursors(const stdx::unordered_set<ShardId>& shardIds, const ShardTag& tag);

    /**
     * Makes the underlying results merger recognize change stream control events.
     * This is used by v2 change stream readers.
     */
    void recognizeControlEvents();

    /**
     * Sets the current high water mark of the underlying results merger. Notably this allows to set
     * the high water mark to a timestamp earlier than the current high water mark.
     */
    void setHighWaterMark(const BSONObj& highWaterMark);

    void setNextHighWaterMarkDeterminingStrategy(
        NextHighWaterMarkDeterminingStrategyPtr nextHighWaterMarkDeterminer);

    /**
     * Returns true if a cursor to the given shard has already been established.
     */
    bool hasShardId(const std::string& shardId) const {
        return _shardsWithCursors.count(shardId) > 0;
    }

protected:
    GetNextResult doGetNext() final;
    void doDispose() final;
    void doForceSpill() final;

private:
    /**
     * Adds the shard Ids of the given remote cursors into the _shardsWithCursors set.
     */
    void recordRemoteCursorShardIds(const std::vector<RemoteCursor>& remoteCursors);

    const std::shared_ptr<BlockingResultsMerger> _blockingResultsMerger;

    // Specific stats for $mergeCursors stage.
    DocumentSourceMergeCursorsStats _stats;

    // Set containing shard ids with valid cursors.
    std::set<ShardId> _shardsWithCursors;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
