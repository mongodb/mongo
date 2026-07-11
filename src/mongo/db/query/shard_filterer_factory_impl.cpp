// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/shard_filterer_factory_impl.h"

#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"

namespace mongo {

std::unique_ptr<ShardFilterer> ShardFiltererFactoryImpl::makeShardFilterer(
    OperationContext* opCtx) const {
    auto scopedCss =
        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, _collection->ns());
    return std::make_unique<ShardFiltererImpl>(scopedCss->getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup));
}

void populateShardFiltererSlot(OperationContext* opCtx,
                               sbe::RuntimeEnvironment& env,
                               sbe::value::SlotId shardFiltererSlot,
                               const MultipleCollectionAccessor& collections) {
    auto shardFilterer = [&]() -> std::unique_ptr<ShardFilterer> {
        const auto& acquisition = collections.getMainCollectionAcquisition();
        tassert(7900701,
                "Setting shard filterer slot on un-sharded collection",
                acquisition.getShardingDescription().isSharded());
        return std::make_unique<ShardFiltererImpl>(*acquisition.getShardingFilter());
    }();
    env.resetSlot(shardFiltererSlot,
                  sbe::value::TypeTags::shardFilterer,
                  sbe::value::bitcastFrom<ShardFilterer*>(shardFilterer.release()),
                  true);
}

}  // namespace mongo
