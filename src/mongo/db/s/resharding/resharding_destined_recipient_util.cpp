// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_destined_recipient_util.h"

#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/sharding_environment/grid.h"

namespace mongo::resharding {

boost::optional<DestinedRecipients> getDestinedRecipientsIfPossiblyDifferent(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& oldDoc,
    const BSONObj& newDoc) {
    const auto& donorReshardingMetadata =
        LocalReshardingOperationsRegistry::get().getDonorMetadata(nss);
    if (!donorReshardingMetadata) {
        return boost::none;
    }
    ShardKeyPattern reshardingKeyPattern(donorReshardingMetadata->getReshardingKey());
    auto oldShardKey = reshardingKeyPattern.extractShardKeyFromDoc(oldDoc);
    auto newShardKey = reshardingKeyPattern.extractShardKeyFromDoc(newDoc);
    if (newShardKey.binaryEqual(oldShardKey))
        return boost::none;
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(
        opCtx, donorReshardingMetadata->getTempReshardingNss(), true));
    if (!cri.hasRoutingTable())
        return boost::none;
    auto chunkMgr = cri.getCurrentChunkManager();
    auto oldRecipShard =
        chunkMgr.findIntersectingChunkWithSimpleCollation(oldShardKey).getShardId();
    auto newRecipShard =
        chunkMgr.findIntersectingChunkWithSimpleCollation(newShardKey).getShardId();
    return DestinedRecipients{oldRecipShard, newRecipShard};
}

}  // namespace mongo::resharding
