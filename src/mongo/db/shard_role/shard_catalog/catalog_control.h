// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <map>

namespace mongo {
namespace catalog {

using MinVisibleTimestamp [[MONGO_MOD_PRIVATE]] = Timestamp;
using MinVisibleTimestampMap [[MONGO_MOD_PRIVATE]] = std::map<UUID, MinVisibleTimestamp>;
using RequiresTimestampExtendedRangeSupportMap [[MONGO_MOD_PRIVATE]] = std::map<UUID, bool>;

struct [[MONGO_MOD_PRIVATE]] PreviousCatalogState {
    MinVisibleTimestampMap minValidTimestampMap;
    RequiresTimestampExtendedRangeSupportMap requiresTimestampExtendedRangeSupportMap;
};

/**
 * Closes the catalog, destroying all associated in-memory data structures for all databases. After
 * a call to this function, it is illegal to access the catalog before calling one of the catalog
 * opening functions (e.g. openCatalogAfterRollbackToStable or openCatalogAfterStorageChange).
 *
 * Must be called with the global lock acquired in exclusive mode.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] PreviousCatalogState closeCatalog(OperationContext* opCtx);

/**
 * Restores the catalog and all in-memory state after a call to closeCatalog(). Used by replication
 * after it recovers to the stable timestamp, whereas initial sync goes through a different sequence
 * that reinitializes storage engine and restores the catalog, and so does not use this function.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void openCatalogAfterRollbackToStable(
    OperationContext* opCtx, const PreviousCatalogState& catalogState, Timestamp stableTimestamp);

/**
 * Restores the catalog and all in-memory state after a call to
 * closeCatalog -> reinitializeStorageEngine -> startupRecovery.
 *
 * Must be called with the global lock acquired in exclusive mode.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void openCatalogAfterStorageChange(OperationContext* opCtx);

}  // namespace catalog
}  // namespace mongo
