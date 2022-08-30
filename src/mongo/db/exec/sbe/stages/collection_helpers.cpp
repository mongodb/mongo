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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/collection_helpers.h"

#include "mongo/db/catalog/collection_catalog.h"

namespace mongo::sbe {

std::tuple<CollectionPtr, NamespaceString, uint64_t> acquireCollection(OperationContext* opCtx,
                                                                       const UUID& collUuid) {
    // The collection is either locked at a higher level or a snapshot of the catalog (consistent
    // with the storage engine snapshot from which we are reading) has been stashed on the
    // 'OperationContext'. Either way, this means that the UUID must still exist in our view of the
    // collection catalog.
    CollectionPtr collPtr = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, collUuid);
    tassert(5071000, str::stream() << "Collection uuid " << collUuid << " does not exist", collPtr);

    auto nss = collPtr->ns();
    return std::make_tuple(
        std::move(collPtr), std::move(nss), CollectionCatalog::get(opCtx)->getEpoch());
}

CollectionPtr restoreCollection(OperationContext* opCtx,
                                const NamespaceString& collName,
                                const UUID& collUuid,
                                uint64_t catalogEpoch) {
    // Re-lookup the collection pointer, by UUID. If the collection has been dropped, then this UUID
    // lookup will result in a null pointer. If the collection has been renamed, then the resulting
    // collection object should have a different name from the original 'collName'. In either
    // scenario, we throw a 'QueryPlanKilled' error and terminate the query.
    CollectionPtr collPtr = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, collUuid);
    if (!collPtr) {
        PlanYieldPolicy::throwCollectionDroppedError(collUuid);
    }

    if (collName != collPtr->ns()) {
        PlanYieldPolicy::throwCollectionRenamedError(collName, collPtr->ns(), collUuid);
    }

    uassert(ErrorCodes::QueryPlanKilled,
            "the catalog was closed and reopened",
            CollectionCatalog::get(opCtx)->getEpoch() == catalogEpoch);

    return collPtr;
}

}  // namespace mongo::sbe
