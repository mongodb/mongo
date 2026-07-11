// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * Rebuilds the indexes on the given collection.
 * One example usage is when a 'dropIndex' command is rolled back. The dropped index must be remade.
 * This function will delete corrupt records when found.
 */
Status rebuildIndexesOnCollection(OperationContext* opCtx, CollectionWriter& collWriter);

}  // namespace mongo
