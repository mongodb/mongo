/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/views/view.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

MONGO_MOD_PUBLIC;

namespace mongo::timeseries {

/**
 * Shard catalog upgrade/downgrade between viewful and viewless timeseries.
 *
 * For both upgrade and downgrade, `mainNs` is the main namespace of the timeseries collection
 * (i.e. without the 'system.buckets' prefix).
 *
 * Do not call this function while holding collection-level locks, since it acquires its own locks
 * internally, so the previously acquired locks may lead to a lock ordering violation.
 *
 * The caller must ensure the backing collection (when upgrading, the 'system.buckets' collection)
 * exists, has timeseries options, and that neither the view nor buckets namespaces are concurrently
 * modified or dropped. This must be done via higher-level means than collection locks
 * (for example, via the DDL lock, or since oplog command entries are applied individually).
 *
 * The caller must only call the upgrade function with the viewless timeseries feature flag enabled.
 * This ensures no new viewful collections can be created while upgrading, so after setFCV finishes
 * all collections are viewless. Similarly, the feature flag must be disabled for downgrading.
 *
 * It is possible that an upgrade is not possible because the collection or view is malformed
 * or there is a conflicting namespace; in this case the upgrade is skipped (i.e. a no-op).
 * Downgrading is always possible.
 *
 * Considerations for oplog application:
 * - Both upgrade and downgrade are idempotent.
 * - The `expectedUUID` parameter is used to check that the targeted namespace has the expected
 *   incarnation of the collection, which may not be the case during initial sync oplog application.
 */
void upgradeToViewlessTimeseries(OperationContext* opCtx,
                                 const NamespaceString& mainNs,
                                 const boost::optional<UUID>& expectedUUID = boost::none);
void downgradeFromViewlessTimeseries(OperationContext* opCtx,
                                     const NamespaceString& mainNs,
                                     const boost::optional<UUID>& expectedUUID = boost::none);

/**
 * Bulk upgrade/downgrade over all collections in the shard catalog.
 */
void upgradeAllTimeseriesToViewless(OperationContext* opCtx);
void downgradeAllTimeseriesFromViewless(OperationContext* opCtx);

/**
 * Validate if a viewful timeseries collection is well-formed.
 *
 * `bucketsColl` is the system.buckets namespace of the collection (which must exist),
 * while `view` is the view on the main namespace (or `nullptr` if it does not exist).
 *
 * To detect the inconsistency where there is a conflicting collection on the main namespace,
 * `mainColl` is the corresponding collection on the main namespace if one exists.
 *
 * If `ensureViewExists` is false, we tolerate that a timeseries view does not exist on the main NS.
 * This is used in two cases:
 * - When validating non-DB primary shards, since only the DB primary shard has the view.
 * - We allow upgrading to viewless timeseries if just the view is missing.
 */
struct BucketsCollectionInconsistency {
    std::string issue;
    BSONObj options;
};

std::vector<BucketsCollectionInconsistency> checkBucketCollectionInconsistencies(
    OperationContext* opCtx,
    const CollectionPtr& bucketsColl,
    bool ensureViewExists,
    const ViewDefinition* view,
    const Collection* mainColl);

}  // namespace mongo::timeseries
