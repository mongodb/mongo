/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/s/query/exec/blocking_results_merger.h"
#include "mongo/s/query/exec/shard_tag.h"
#include "mongo/stdx/unordered_set.h"

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
     * Adds the specified, already opened cursors for remote shards or the config server.
     */
    void addNewShardCursors(std::vector<RemoteCursor>&& newCursors,
                            const ShardTag& tag = ShardTag::kDefault);

    /**
     * Close the set of specified open cursors on remote shards and/or the config server.
     * This is used by v2 change stream readers.
     */
    void closeShardCursors(const stdx::unordered_set<ShardId>& shardIds, const ShardTag& tag);

    /**
     * Make the underlying results merger recognize change stream control events.
     * This is used by v2 change stream readers.
     */
    void recognizeControlEvents();

    void setInitialHighWaterMark(const BSONObj& highWaterMark);

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
