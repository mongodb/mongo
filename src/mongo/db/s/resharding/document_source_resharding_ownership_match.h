/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

/**
 * This is a purpose-built stage to filter out documents which are 'owned' by this shard according
 * to a given shardId and shard key. This stage was created to optimize performance of internal
 * resharding pipelines which need to be able to answer this question very quickly. To do so, it
 * re-uses pieces of sharding infrastructure rather than applying a MatchExpression.
 */
class DocumentSourceReshardingOwnershipMatch final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalReshardingOwnershipMatch"_sd;

    static boost::intrusive_ptr<DocumentSourceReshardingOwnershipMatch> create(
        ShardId recipientShardId,
        ShardKeyPattern reshardingKey,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceReshardingOwnershipMatch> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const final {
        return DocumentSourceReshardingOwnershipMatch::kStageName.rawData();
    }

private:
    DocumentSourceReshardingOwnershipMatch(ShardId recipientShardId,
                                           ShardKeyPattern reshardingKey,
                                           const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSource::GetNextResult doGetNext() final;

    const ShardId _recipientShardId;
    const ShardKeyPattern _reshardingKey;

    // _tempReshardingChunkMgr is used to decide to which recipient shard that documents in the
    // source collection should be routed. It is safe to cache this information for the duration of
    // the aggregation pipeline because the ownership information for the temporary resharding
    // collection is frozen for the duration of the resharding operation.
    boost::optional<ChunkManager> _tempReshardingChunkMgr;
};

}  // namespace mongo
