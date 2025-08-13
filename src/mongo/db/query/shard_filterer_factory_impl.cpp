/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/shard_filterer_factory_impl.h"

#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"

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
        if (collections.isAcquisition()) {
            const auto& acquisition = collections.getMainCollectionAcquisition();
            tassert(7900701,
                    "Setting shard filterer slot on un-sharded collection",
                    acquisition.getShardingDescription().isSharded());
            return std::make_unique<ShardFiltererImpl>(*acquisition.getShardingFilter());
        } else {
            const auto& collection = collections.getMainCollection();
            tassert(6108307,
                    "Setting shard filterer slot on un-sharded collection",
                    collection.isSharded_DEPRECATED());
            ShardFiltererFactoryImpl shardFiltererFactory(collection);
            return shardFiltererFactory.makeShardFilterer(opCtx);
        }
    }();
    env.resetSlot(shardFiltererSlot,
                  sbe::value::TypeTags::shardFilterer,
                  sbe::value::bitcastFrom<ShardFilterer*>(shardFilterer.release()),
                  true);
}

}  // namespace mongo
