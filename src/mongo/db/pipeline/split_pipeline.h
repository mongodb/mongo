// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace sharded_agg_helpers {

/**
 * Represents the two halves of a pipeline that will execute in a sharded cluster. 'shardsPipeline'
 * will execute in parallel on each shard, and 'mergePipeline' will execute on the merge host -
 * either one of the shards or a mongos.
 */
class [[MONGO_MOD_PUBLIC]] SplitPipeline {
public:
    /**
     * Split the given Pipeline into a Pipeline for each shard, and a Pipeline that combines the
     * results within a merging process. This call also performs optimizations with the aim of
     * reducing computing time and network traffic when a pipeline has been split into two pieces.
     *
     * The 'shardKeyPaths' represent the initial shard key at the start of the pipeline, which can
     * be used to make splitting decisions (e.g. whether or not a $group can be pushed down to the
     * shards).
     *
     * The 'mergePipeline' returned as part of the SplitPipeline here is not ready to execute until
     * the 'shardsPipeline' has been sent to the shards and cursors have been established. Once
     * cursors have been established, the merge pipeline can be made executable by calling
     * 'addMergeCursorsSource()'.
     */
    static SplitPipeline split(std::unique_ptr<Pipeline> pipelineToSplit,
                               boost::optional<OrderedPathSet> shardKeyPaths = boost::none);

    /**
     * Creates a SplitPipeline using the given pipeline as the 'mergePipeline', where the
     * 'shardsPipeline' is null.
     */
    static SplitPipeline mergeOnly(std::unique_ptr<Pipeline> mergePipeline) {
        return SplitPipeline(nullptr, std::move(mergePipeline), boost::none);
    }

    /**
     * Creates a SplitPipeline using the given pipeline as the 'shardsPipeline', where the
     * 'mergePipeline' is null.
     */
    static SplitPipeline shardsOnly(std::unique_ptr<Pipeline> shardsPipeline) {
        return SplitPipeline(std::move(shardsPipeline), nullptr, boost::none);
    }

    /**
     * Creates a SplitPipeline using the given pipeline as the 'mergePipeline', where the
     * 'shardsPipeline' is an empty pipeline.
     */
    static SplitPipeline mergeOnlyWithEmptyShardsPipeline(std::unique_ptr<Pipeline> mergePipeline) {
        const auto& expCtx = mergePipeline->getContext();
        return SplitPipeline(Pipeline::create({}, expCtx), std::move(mergePipeline), boost::none);
    }

    std::unique_ptr<Pipeline> shardsPipeline;
    std::unique_ptr<Pipeline> mergePipeline;

    // If set, the cursors from the shards are expected to be sorted according to this spec, and to
    // have populated a "$sortKey" metadata field which can be used to compare the results.
    boost::optional<BSONObj> shardCursorsSortSpec;

private:
    SplitPipeline(std::unique_ptr<Pipeline> shardsPipeline,
                  std::unique_ptr<Pipeline> mergePipeline,
                  boost::optional<BSONObj> shardCursorsSortSpec)
        : shardsPipeline(std::move(shardsPipeline)),
          mergePipeline(std::move(mergePipeline)),
          shardCursorsSortSpec(std::move(shardCursorsSortSpec)) {}
};

}  // namespace sharded_agg_helpers
}  // namespace mongo
