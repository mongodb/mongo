// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_yield_restore.h"

#include "mongo/db/shard_role/direct_connection_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/snapshot_helper.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {
namespace {

bool locked(OperationContext* opCtx, const NamespaceString& ns) {
    if (ns.isEmpty()) {
        return true;
    }

    const auto& locker = shard_role_details::getLocker(opCtx);
    if (ns.isOplog()) {
        return locker->isReadLocked();
    }

    return locker->isCollectionLockedForMode(ns, MODE_IS);
}

}  // namespace

LockedCollectionYieldRestore::LockedCollectionYieldRestore(OperationContext* opCtx,
                                                           const CollectionPtr& coll)
    : _nss(coll ? coll->ns() : NamespaceString::kEmpty) {
    invariant(locked(opCtx, _nss));
}

ConsistentCollection LockedCollectionYieldRestore::operator()(OperationContext* opCtx,
                                                              boost::optional<UUID> optUuid) const {
    // Confirm that we were set with a valid collection instance at construction if yield is
    // performed.
    invariant(!_nss.isEmpty());
    // Confirm that we are holding the necessary collection level lock.
    invariant(locked(opCtx, _nss));

    // If no UUID was provided, just do nothing.
    if (!optUuid) {
        return ConsistentCollection{};
    }
    const auto& uuid = *optUuid;

    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);

    // Fetch the Collection by UUID. A rename could have occurred which means we might not be
    // holding the collection-level lock on the right namespace.
    auto collection = catalog->lookupCollectionByUUID(opCtx, uuid);

    // Collection dropped during yielding.
    if (!collection) {
        return ConsistentCollection{};
    }

    // Collection renamed during yielding.
    // This check ensures that we are locked on the same namespace and that it is safe to return
    // the C-style pointer to the Collection.
    if (collection->ns() != _nss) {
        return ConsistentCollection{};
    }

    // Check if this operation is a direct connection and if it is authorized to be one.
    direct_connection_util::checkDirectShardOperationAllowed(opCtx, _nss);

    // After yielding and reacquiring locks, the preconditions that were used to select our
    // ReadSource initially need to be checked again. We select a ReadSource based on replication
    // state. After a query yields its locks, the replication state may have changed, invalidating
    // our current choice of ReadSource. Using the same preconditions, change our ReadSource if
    // necessary.
    auto readSourceInfo = SnapshotHelper::getReadSourceForSecondaryReadsIfNeeded(opCtx, _nss);
    if (readSourceInfo) {
        SnapshotHelper::updateReadSourceTimestampForSecondaryReadsIfPossible(
            opCtx, _nss, *readSourceInfo);
    }

    return ConsistentCollection{opCtx, collection};
}

}  // namespace mongo
