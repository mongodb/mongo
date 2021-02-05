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

namespace mongo::sbe {

NamespaceString acquireCollection(OperationContext* opCtx,
                                  CollectionUUID collUuid,
                                  const LockAcquisitionCallback& lockAcquisitionCallback,
                                  boost::optional<AutoGetCollectionForReadMaybeLockFree>& coll) {
    tassert(5071012, "cannot restore 'coll' if already held", !coll.has_value());
    // The caller is expected to hold the appropriate db_raii object to give us a consistent view of
    // the catalog, so the UUID must still exist.
    auto collName = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, collUuid);
    tassert(5071000,
            str::stream() << "Collection uuid " << collUuid << " does not exist",
            collName.has_value());

    coll.emplace(opCtx, NamespaceStringOrUUID{collName->db().toString(), collUuid});
    if (lockAcquisitionCallback) {
        lockAcquisitionCallback(opCtx, *coll);
    }

    return *collName;
}

void restoreCollection(OperationContext* opCtx,
                       const NamespaceString& collName,
                       CollectionUUID collUuid,
                       const LockAcquisitionCallback& lockAcquisitionCallback,
                       boost::optional<AutoGetCollectionForReadMaybeLockFree>& coll) {
    tassert(5071011, "cannot restore 'coll' if already held", !coll.has_value());
    // Reaquire the AutoGet object, looking up in the catalog by UUID. If the collection has been
    // dropped, then this UUID lookup will fail, throwing an exception and terminating the query.
    NamespaceStringOrUUID nssOrUuid{collName.db().toString(), collUuid};
    try {
        coll.emplace(opCtx, nssOrUuid);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        ex.addContext(str::stream() << "collection dropped: '" << collName << "'");
        throw;
    }

    if (collName != coll->getNss()) {
        PlanYieldPolicy::throwCollectionRenamedError(collName, coll->getNss(), collUuid);
    }

    if (lockAcquisitionCallback) {
        lockAcquisitionCallback(opCtx, *coll);
    }
}

}  // namespace mongo::sbe
