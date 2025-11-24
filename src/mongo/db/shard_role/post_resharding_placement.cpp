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
    _tmpReshardingCollectionChunkManager = tmpNssRoutingInfoWithStatus.getValue().getChunkManager();
}

const ShardId& PostReshardingCollectionPlacement::getReshardingDestinedRecipient(
    const BSONObj& fullDocument) const {
    auto newShardKey = extractReshardingKeyFromDocument(fullDocument);
    return getReshardingDestinedRecipientFromShardKey(newShardKey);
}

BSONObj PostReshardingCollectionPlacement::extractReshardingKeyFromDocument(
    const BSONObj& fullDocument) const {
    return _reshardingKeyPattern->extractShardKeyFromDocThrows(fullDocument);
}

const ShardId& PostReshardingCollectionPlacement::getReshardingDestinedRecipientFromShardKey(
    const BSONObj& reshardingKey) const {
    return _tmpReshardingCollectionChunkManager
        ->findIntersectingChunkWithSimpleCollation(reshardingKey)
        .getShardId();
}
}  // namespace mongo
