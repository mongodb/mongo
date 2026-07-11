// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/post_resharding_placement.h"

#include "mongo/db/sharding_environment/grid.h"

namespace mongo {

PostReshardingCollectionPlacement::PostReshardingCollectionPlacement(
    OperationContext* opCtx, const ScopedCollectionDescription& collectionDescription) {
    _reshardingKeyPattern = collectionDescription.getReshardingKeyIfShouldForwardOps();
    tassert(11178200,
            "Attempting to create a PostReshardingCollectionPlacement without reshardingKeyPattern",
            _reshardingKeyPattern);
    const auto& reshardingFields = collectionDescription.getReshardingFields();
    tassert(11178201,
            "Found a sharded collection with a resharding key pattern but not the "
            "resharding fields",
            reshardingFields);
    const auto& donorFields = reshardingFields->getDonorFields();
    tassert(11178202,
            "Found a sharded collection with a resharding key pattern and the resharding "
            "fields but not the donor fields",
            donorFields);
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    invariant(catalogCache);
    auto tmpNssRoutingInfoWithStatus = catalogCache->getCollectionRoutingInfo(
        opCtx, donorFields->getTempReshardingNss(), true /* allowLocks */);
    uassertStatusOK(tmpNssRoutingInfoWithStatus);
    tassert(11178203,
            "Routing information for the temporary resharding collection is stale",
            tmpNssRoutingInfoWithStatus.getValue().hasRoutingTable());
    _tmpReshardingCollectionChunkManager =
        tmpNssRoutingInfoWithStatus.getValue().getCurrentChunkManager();
}

const ShardRef& PostReshardingCollectionPlacement::getReshardingDestinedRecipient(
    const BSONObj& fullDocument) const {
    auto newShardKey = extractReshardingKeyFromDocument(fullDocument);
    return getReshardingDestinedRecipientFromShardKey(newShardKey);
}

BSONObj PostReshardingCollectionPlacement::extractReshardingKeyFromDocument(
    const BSONObj& fullDocument) const {
    return _reshardingKeyPattern->extractShardKeyFromDocThrows(fullDocument);
}

const ShardRef& PostReshardingCollectionPlacement::getReshardingDestinedRecipientFromShardKey(
    const BSONObj& reshardingKey) const {
    return _tmpReshardingCollectionChunkManager
        ->findIntersectingChunkWithSimpleCollation(reshardingKey)
        .getShardRef();
}
}  // namespace mongo
