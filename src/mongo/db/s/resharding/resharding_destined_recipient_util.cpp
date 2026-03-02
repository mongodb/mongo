/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_destined_recipient_util.h"

#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/shard_role/post_resharding_placement.h"
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

boost::optional<DestinedRecipients> getDestinedRecipientsIfPossiblyDifferent(
    OperationContext* opCtx,
    const boost::optional<PostReshardingCollectionPlacement>& reshardingPlacement,
    const BSONObj& oldDoc,
    const BSONObj& newDoc) {
    if (!reshardingPlacement)
        return boost::none;
    auto oldShardKey = reshardingPlacement->extractReshardingKeyFromDocument(oldDoc);
    auto newShardKey = reshardingPlacement->extractReshardingKeyFromDocument(newDoc);
    if (newShardKey.binaryEqual(oldShardKey))
        return boost::none;
    auto oldRecipShard =
        reshardingPlacement->getReshardingDestinedRecipientFromShardKey(oldShardKey);
    auto newRecipShard =
        reshardingPlacement->getReshardingDestinedRecipientFromShardKey(newShardKey);
    return DestinedRecipients{oldRecipShard, newRecipShard};
}

}  // namespace mongo::resharding
