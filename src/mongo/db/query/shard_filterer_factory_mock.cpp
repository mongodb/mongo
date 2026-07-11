// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/shard_filterer_factory_mock.h"

#include <utility>

namespace mongo {

ShardFiltererFactoryMock::ShardFiltererFactoryMock(std::unique_ptr<ShardFilterer> shardFilterer)
    : _shardFilterer(std::move(shardFilterer)) {}

std::unique_ptr<ShardFilterer> ShardFiltererFactoryMock::makeShardFilterer(
    OperationContext* opCtx) const {
    return _shardFilterer->clone();
}
}  // namespace mongo
