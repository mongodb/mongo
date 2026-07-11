// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * An implementation of ShardFiltererFactoryInterface.
 */
class ShardFiltererFactoryImpl : public ShardFiltererFactoryInterface {
public:
    ShardFiltererFactoryImpl(const CollectionPtr& collection) : _collection(collection) {}

    std::unique_ptr<ShardFilterer> makeShardFilterer(OperationContext* opCtx) const override;

private:
    const CollectionPtr& _collection;
};

/**
 * Construct a ShardFilterer for the main collection from 'collections' and populate the slot
 * specified by 'slotId' in 'env' with it.
 */
void populateShardFiltererSlot(OperationContext* opCtx,
                               sbe::RuntimeEnvironment& env,
                               sbe::value::SlotId shardFiltererSlot,
                               const MultipleCollectionAccessor& collections);

}  // namespace mongo
