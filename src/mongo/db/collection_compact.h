// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/storage/compact_options.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

/**
 * Compacts collection.
 *
 * Returns the number of bytes of stable storage and index size that were freed. If the total
 * size decreased, the return value is positive. Otherwise, the return value is negative.
 */
[[MONGO_MOD_PRIVATE]]
StatusWith<int64_t> compactCollection(OperationContext* opCtx,
                                      const CompactOptions& options,
                                      const CollectionPtr& collection);

}  // namespace mongo
