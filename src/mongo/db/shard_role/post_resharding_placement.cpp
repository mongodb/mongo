// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/post_resharding_placement.h"

#include "mongo/db/sharding_environment/grid.h"

namespace mongo {

PostReshardingCollectionPlacement::PostReshardingCollectionPlacement(
    OperationContext* opCtx, const ScopedCollectionDescription& collectionDescription) {
    _reshardingKeyPattern = collectionDescription.getReshardingKeyIfShouldForwardOps();
    if (!_reshardingKeyPattern) {
        _invalidReason = "PostReshardingCollectionPlacement created without reshardingKeyPattern";
        return;
    }

    const auto& reshardingFields = collectionDescription.getReshardingFields();

    if (!reshardingFields) {
        _invalidReason =
            "Found a sharded collection with a resharding key pattern but not the resharding "
            "fields";
        return;
    }

    const auto& donorFields = reshardingFields->getDonorFields();
    if (!donorFields) {
        _invalidReason =
            "Found a sharded collection with a resharding key pattern and the resharding fields "
            "but not the donor fields";
        return;
    }

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    invariant(catalogCache);
    auto tmpNssRoutingInfoWithStatus = catalogCache->getCollectionRoutingInfo(
        opCtx, donorFields->getTempReshardingNss(), true /* allowLocks */);
    uassertStatusOK(tmpNssRoutingInfoWithStatus);

    if (!tmpNssRoutingInfoWithStatus.getValue().hasRoutingTable()) {
        _invalidReason = "Routing information for the temporary resharding collection is stale";
        return;
    }

    _tmpReshardingCollectionChunkManager =
        tmpNssRoutingInfoWithStatus.getValue().getCurrentChunkManager();
}

const ShardRef& PostReshardingCollectionPlacement::getReshardingDestinedRecipient(
    const BSONObj& fullDocument) const {
    _checkIsValid();
    auto newShardKey = extractReshardingKeyFromDocument(fullDocument);
    return getReshardingDestinedRecipientFromShardKey(newShardKey);
}

BSONObj PostReshardingCollectionPlacement::extractReshardingKeyFromDocument(
    const BSONObj& fullDocument) const {
    _checkIsValid();
    return _reshardingKeyPattern->extractShardKeyFromDocThrows(fullDocument);
}

const ShardRef& PostReshardingCollectionPlacement::getReshardingDestinedRecipientFromShardKey(
    const BSONObj& reshardingKey) const {
    _checkIsValid();
    return _tmpReshardingCollectionChunkManager
        ->findIntersectingChunkWithSimpleCollation(reshardingKey)
        .getShardRef();
}

void PostReshardingCollectionPlacement::_checkIsValid() const {
    tassert(13150300, _invalidReason, _invalidReason.empty());
}

}  // namespace mongo
