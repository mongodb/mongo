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

#include "mongo/db/s/sharding_write_router.h"

namespace mongo {

ShardingWriteRouter::ShardingWriteRouter(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         CatalogCache* catalogCache)
    : _scopedCss(CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)),
      _collDesc(_scopedCss->getCollectionDescription(opCtx)) {
    if (!_collDesc.isSharded()) {
        invariant(!_collDesc.getReshardingKeyIfShouldForwardOps());
        return;
    }

    _reshardingKeyPattern = _collDesc.getReshardingKeyIfShouldForwardOps();
    if (_reshardingKeyPattern) {
        _ownershipFilter = _scopedCss->getOwnershipFilter(
            opCtx, CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);

        const auto& reshardingFields = _collDesc.getReshardingFields();
        invariant(reshardingFields);
        const auto& donorFields = reshardingFields->getDonorFields();
        invariant(donorFields);

        _reshardingChunkMgr = uassertStatusOK(catalogCache->getCollectionRoutingInfo(
            opCtx, donorFields->getTempReshardingNss(), true /* allowLocks */));

        tassert(6862800,
                "Routing information for the temporary resharing collection is stale",
                _reshardingChunkMgr->isSharded());
    }
}

boost::optional<ShardId> ShardingWriteRouter::getReshardingDestinedRecipient(
    const BSONObj& fullDocument) const {
    if (!_reshardingKeyPattern) {
        return boost::none;
    }

    invariant(_ownershipFilter);
    invariant(_reshardingChunkMgr);

    const auto& shardKeyPattern = _collDesc.getShardKeyPattern();
    if (!_ownershipFilter->keyBelongsToMe(shardKeyPattern.extractShardKeyFromDoc(fullDocument))) {
        return boost::none;
    }

    auto shardKey = _reshardingKeyPattern->extractShardKeyFromDocThrows(fullDocument);
    return _reshardingChunkMgr->findIntersectingChunkWithSimpleCollation(shardKey).getShardId();
}

}  // namespace mongo
