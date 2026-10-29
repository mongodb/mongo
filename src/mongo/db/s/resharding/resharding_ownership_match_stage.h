// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the resharding ownership match aggregation stage and is
 * part of the execution pipeline. Its construction is based on
 * DocumentSourceReshardingOwnershipMatch, which handles the optimization part.
 */
class ReshardingOwnershipMatchStage final : public Stage {
public:
    ReshardingOwnershipMatchStage(std::string_view stageName,
                                  const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  ShardId recipientShardId,
                                  const std::shared_ptr<ShardKeyPattern>& reshardingKey,
                                  boost::optional<NamespaceString> temporaryReshardingNamespace);

private:
    GetNextResult doGetNext() final;

    // TODO SERVER-105521: Check if we can std::move '_recipientShardId', '_reshardingKey' and
    // '_temporaryReshardingNamespace' to ReshardingOwnershipMatchStage, instead of copy.
    const ShardId _recipientShardId;
    std::shared_ptr<ShardKeyPattern> _reshardingKey;
    const boost::optional<NamespaceString> _temporaryReshardingNamespace;

    // _tempReshardingChunkMgr is used to decide to which recipient shard that documents in the
    // source collection should be routed. It is safe to cache this information for the duration of
    // the aggregation pipeline because the ownership information for the temporary resharding
    // collection is frozen for the duration of the resharding operation.
    boost::optional<ChunkManager> _tempReshardingChunkMgr;
};

}  // namespace mongo::exec::agg
