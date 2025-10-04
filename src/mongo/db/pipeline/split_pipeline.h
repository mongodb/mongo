/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace sharded_agg_helpers {

/**
 * Represents the two halves of a pipeline that will execute in a sharded cluster. 'shardsPipeline'
 * will execute in parallel on each shard, and 'mergePipeline' will execute on the merge host -
 * either one of the shards or a mongos.
 */
class SplitPipeline {
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
