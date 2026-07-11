// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * An implementation of ShardFiltererFactoryInterface for unit testing.
 */
class ShardFiltererFactoryMock : public ShardFiltererFactoryInterface {
public:
    ShardFiltererFactoryMock(std::unique_ptr<ShardFilterer> shardFilter);

    /*
     * Makes a new mock ShardFilterer.
     */
    std::unique_ptr<ShardFilterer> makeShardFilterer(OperationContext* opCtx) const override;

private:
    std::unique_ptr<ShardFilterer> _shardFilterer;
};
}  // namespace mongo
