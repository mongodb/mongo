/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/catalog_helper.h"

#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>

namespace mongo::catalog_helper {
MONGO_FAIL_POINT_DEFINE(setAutoGetCollectionWait);

StorageEngine::TimestampMonitor::TimestampListener kCollectionCatalogCleanupTimestampListener(
    StorageEngine::TimestampMonitor::TimestampType::kOldest,
    [](OperationContext* opCtx, Timestamp timestamp) {
        if (CollectionCatalog::latest(opCtx)->catalogIdTracker().dirty(timestamp)) {
            CollectionCatalog::write(opCtx, [timestamp](CollectionCatalog& catalog) {
                catalog.catalogIdTracker().cleanup(timestamp);
            });
        }
    });

namespace {
/**
 * Defines sorting order for NamespaceStrings based on what their ResourceId would be for locking.
 */
struct ResourceIdNssComparator {
    bool operator()(const NamespaceString& lhs, const NamespaceString& rhs) const {
        return ResourceId(RESOURCE_COLLECTION, lhs) < ResourceId(RESOURCE_COLLECTION, rhs);
    }
};
}  // namespace

void acquireCollectionLocksInResourceIdOrder(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    LockMode modeColl,
    Date_t deadline,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryNssOrUUIDsBegin,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryNssOrUUIDsEnd,
    std::vector<CollectionNamespaceOrUUIDLock>* collLocks) {
    invariant(collLocks->empty());

    // Optimisation for single lock requests. CollectionNamespaceOrUUIDLock has the same logic
    // internally as this method for UUID lookups so it avoids unnecessary memory allocations/work.
    if (secondaryNssOrUUIDsBegin == secondaryNssOrUUIDsEnd) {
        collLocks->emplace_back(opCtx, nsOrUUID, modeColl, deadline);
        return;
    }

    auto catalog = CollectionCatalog::get(opCtx);

    // Use a set so that we can easily dedupe namespaces to avoid locking the same collection twice.
    std::set<NamespaceString, ResourceIdNssComparator> temp;
    std::set<NamespaceString, ResourceIdNssComparator> verifyTemp;
    do {
        // Clear the data structures when/if we loop more than once.
        collLocks->clear();
        temp.clear();
        verifyTemp.clear();

        // Create a single set with all the resolved namespaces sorted by ascending
        // ResourceId(RESOURCE_COLLECTION, nss).
        temp.insert(
            catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(opCtx, nsOrUUID));
        for (auto iter = secondaryNssOrUUIDsBegin; iter != secondaryNssOrUUIDsEnd; ++iter) {
            const auto& secondaryNssOrUUID = *iter;
            invariant(secondaryNssOrUUID.dbName() == nsOrUUID.dbName(),
                      str::stream()
                          << "Unable to acquire locks for collections across different databases ("
                          << secondaryNssOrUUID.toStringForErrorMsg() << " vs "
                          << nsOrUUID.toStringForErrorMsg() << ")");
            temp.insert(catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                opCtx, secondaryNssOrUUID));
        }

        // Acquire all of the locks in order. And clear the 'catalog' because the locks will access
        // a fresher one internally.
        catalog = nullptr;
        for (auto& nss : temp) {
            collLocks->emplace_back(opCtx, nss, modeColl, deadline);
        }

        // Check that the namespaces have NOT changed after acquiring locks. It's possible to race
        // with a rename collection when the given NamespaceStringOrUUID is a UUID, and consequently
        // fail to lock the correct namespace.
        //
        // The catalog reference must be refreshed to see the latest Collection data. Otherwise we
        // won't see any concurrent DDL/catalog operations.
        catalog = CollectionCatalog::get(opCtx);
        verifyTemp.insert(
            catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(opCtx, nsOrUUID));
        for (auto iter = secondaryNssOrUUIDsBegin; iter != secondaryNssOrUUIDsEnd; ++iter) {
            const auto& secondaryNssOrUUID = *iter;
            verifyTemp.insert(catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                opCtx, secondaryNssOrUUID));
        }
    } while (temp != verifyTemp);
}
}  // namespace mongo::catalog_helper
