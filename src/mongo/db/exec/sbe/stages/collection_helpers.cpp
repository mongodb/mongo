/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/collection_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {

CollectionPtr CollectionRef::getConsistentCollection(OperationContext* opCtx,
                                                     const DatabaseName& dbName,
                                                     const UUID& collUuid) {
    auto timestamp = shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
    return CollectionPtr{CollectionCatalog::get(opCtx)->establishConsistentCollection(
        opCtx, NamespaceStringOrUUID{dbName, collUuid}, timestamp)};
}

void CollectionRef::acquireCollection(OperationContext* opCtx,
                                      const DatabaseName& dbName,
                                      const UUID& collUuid) {
    // The collection is either locked at a higher level or a snapshot of the catalog (consistent
    // with the storage engine snapshot from which we are reading) has been stashed on the
    // 'OperationContext'. Either way, this means that the UUID must still exist in our view of the
    // collection catalog.
    if (!isAcquisition()) {
        tassert(
            9367600, "'_coll' should not be initialized prior to 'acquireCollection()'", !(*this));
        CollectionPtr collPtr = getConsistentCollection(opCtx, dbName, collUuid);

        // Perform any work that might throw before updating '_collPtr', because in the event an
        // exception is thrown we don't want '_collPtr' to be holding a CollectionPtr.
        tassert(
            5071000, str::stream() << "Collection uuid " << collUuid << " does not exist", collPtr);

        _collName = collPtr->ns();
        _catalogEpoch = CollectionCatalog::get(opCtx)->getEpoch();
        _collPtr.emplace(std::move(collPtr));
    }
}

void CollectionRef::restoreCollection(OperationContext* opCtx,
                                      const DatabaseName& dbName,
                                      const UUID& collUuid) {
    tassert(5777401, "Collection name should be initialized", _collName);
    tassert(5777402, "Catalog epoch should be initialized", _catalogEpoch);

    // Establish a consistent collection instance and restore the collection pointer. If the
    // collection has been dropped, then this UUID lookup will result in a null pointer. If the
    // collection has been renamed, then the resulting collection object should have a different
    // name from the original '_collName'. In either scenario, we throw a 'QueryPlanKilled' error
    // and terminate the query.
    CollectionPtr collPtr = getConsistentCollection(opCtx, dbName, collUuid);

    // Perform any work that might throw before updating '_collPtr', because in the event an
    // exception is thrown we don't want '_collPtr' to be holding a CollectionPtr.
    if (!collPtr) {
        PlanYieldPolicy::throwCollectionDroppedError(collUuid);
    }

    if (*_collName != collPtr->ns()) {
        PlanYieldPolicy::throwCollectionRenamedError(*_collName, collPtr->ns(), collUuid);
    }

    uassert(ErrorCodes::QueryPlanKilled,
            "the catalog was closed and reopened",
            CollectionCatalog::get(opCtx)->getEpoch() == *_catalogEpoch);

    _collPtr.emplace(std::move(collPtr));
}

}  // namespace mongo::sbe
