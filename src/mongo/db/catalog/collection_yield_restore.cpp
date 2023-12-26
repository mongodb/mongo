/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_yield_restore.h"

#include <memory>


#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/storage/capped_snapshots.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {
namespace {

bool locked(OperationContext* opCtx, const NamespaceString& ns) {
    if (ns.isEmpty()) {
        return true;
    }

    if (ns.isOplog()) {
        return shard_role_details::getLocker(opCtx)->isReadLocked();
    }

    if (ns.isChangeCollection() && ns.tenantId()) {
        return shard_role_details::getLocker(opCtx)->isLockHeldForMode(
            {ResourceType::RESOURCE_TENANT, *ns.tenantId()}, MODE_IS);
    }

    return shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(ns, MODE_IS);
}

}  // namespace

LockedCollectionYieldRestore::LockedCollectionYieldRestore(OperationContext* opCtx,
                                                           const CollectionPtr& coll)
    : _nss(coll ? coll->ns() : NamespaceString::kEmpty) {
    invariant(locked(opCtx, _nss));
}

const Collection* LockedCollectionYieldRestore::operator()(OperationContext* opCtx,
                                                           const UUID& uuid) const {
    // Confirm that we were set with a valid collection instance at construction if yield is
    // performed.
    invariant(!_nss.isEmpty());
    // Confirm that we are holding the necessary collection level lock.
    invariant(locked(opCtx, _nss));

    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);

    // Fetch the Collection by UUID. A rename could have occurred which means we might not be
    // holding the collection-level lock on the right namespace.
    auto collection = catalog->lookupCollectionByUUID(opCtx, uuid);

    // Collection dropped during yielding.
    if (!collection) {
        return nullptr;
    }

    // Collection renamed during yielding.
    // This check ensures that we are locked on the same namespace and that it is safe to return
    // the C-style pointer to the Collection.
    if (collection->ns() != _nss) {
        return nullptr;
    }

    // Non-lock-free readers use this path and need to re-establish their capped snapshot.
    if (collection->usesCappedSnapshots()) {
        CappedSnapshots::get(opCtx).establish(opCtx, collection);
    }

    // After yielding and reacquiring locks, the preconditions that were used to select our
    // ReadSource initially need to be checked again. We select a ReadSource based on replication
    // state. After a query yields its locks, the replication state may have changed, invalidating
    // our current choice of ReadSource. Using the same preconditions, change our ReadSource if
    // necessary.
    SnapshotHelper::changeReadSourceIfNeeded(opCtx, collection->ns());

    return collection;
}

}  // namespace mongo
