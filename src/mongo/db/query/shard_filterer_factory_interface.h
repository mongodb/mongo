// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * An interface that can be used to construct a ShardFilterer.
 */
class ShardFiltererFactoryInterface {
public:
    virtual ~ShardFiltererFactoryInterface() = default;

    virtual std::unique_ptr<ShardFilterer> makeShardFilterer(OperationContext* opCtx) const = 0;
};
}  // namespace mongo
