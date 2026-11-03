// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/sharding_write_router.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ShardingWriteRouter::ShardingWriteRouter(OperationContext* opCtx, const NamespaceString& nss) {

    if (!serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        return;
    }

    auto css = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss);
    _collDesc = css->getCollectionDescription(opCtx);

    if (!_collDesc->hasRoutingTable()) {
        return;
    }

    const auto& op = LocalReshardingOperationsRegistry::get().getOperation(nss);

    if (op.has_value() && op->roles.contains(ReshardingMetricsCommon::Role::kDonor)) {
        _reshardingKeyPattern = ShardKeyPattern(op->metadata.getReshardingKey());
        invariant(_reshardingKeyPattern);
        _ownershipFilter = css->getOwnershipFilter(
            opCtx, CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);
        auto catalogCache = Grid::get(opCtx)->catalogCache();
        invariant(catalogCache);

        const auto& cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(
            opCtx, op->metadata.getTempReshardingNss(), /*allowLocks=*/true));
        if (!cri.hasRoutingTable()) {
            // We could reach here if resharding operation has already committed and the temporary
            // collection no longer exists, but the state document hasn't been cleaned up yet in
            // registry. In this case, proceed without computing a destined recipient.
            _reshardingKeyPattern.reset();
            _ownershipFilter.reset();
            return;
        }
        _reshardingChunkMgr = cri.getCurrentChunkManager();
    }
}

boost::optional<ShardId> ShardingWriteRouter::getReshardingDestinedRecipient(
    const BSONObj& fullDocument) const {
    if (!_reshardingKeyPattern) {
        return boost::none;
    }

    invariant(_ownershipFilter);
    invariant(_reshardingChunkMgr);

    const auto& shardKeyPattern = _collDesc->getShardKeyPattern();
    if (!_ownershipFilter->keyBelongsToMe(shardKeyPattern.extractShardKeyFromDoc(fullDocument))) {
        return boost::none;
    }

    auto shardKey = _reshardingKeyPattern->extractShardKeyFromDocThrows(fullDocument);
    return _reshardingChunkMgr->findIntersectingChunkWithSimpleCollation(shardKey).getShardId();
}

}  // namespace mongo
