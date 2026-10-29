// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/catalog_helper.h"

#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"

namespace mongo::catalog_helper {
MONGO_FAIL_POINT_DEFINE(setAutoGetCollectionWait);

StorageEngine::TimestampMonitor::TimestampListener kCollectionCatalogCleanupTimestampListener(
    [](OperationContext* opCtx, const StorageEngine::TimestampMonitor::Timestamps& timestamp) {
        auto oldest = timestamp.oldest;
        if (CollectionCatalog::latest(opCtx)->catalogIdTracker().dirty(oldest)) {
            CollectionCatalog::write(opCtx, [oldest](CollectionCatalog& catalog) {
                catalog.catalogIdTracker().cleanup(oldest);
            });
        }
    });

}  // namespace mongo::catalog_helper
