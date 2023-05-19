/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_catalog_helper.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeGettingNextCollection);

namespace catalog {

Status checkIfNamespaceExists(OperationContext* opCtx, const NamespaceString& nss) {
    auto catalog = CollectionCatalog::get(opCtx);
    if (catalog->lookupCollectionByNamespace(opCtx, nss)) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream()
                          << "Collection " << nss.toStringForErrorMsg() << " already exists.");
    }

    auto view = catalog->lookupView(opCtx, nss);
    if (!view)
        return Status::OK();

    if (view->timeseries()) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "A timeseries collection already exists. NS: "
                                    << nss.toStringForErrorMsg());
    }

    return Status(ErrorCodes::NamespaceExists,
                  str::stream() << "A view already exists. NS: " << nss.toStringForErrorMsg());
}


void forEachCollectionFromDb(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             LockMode collLockMode,
                             CollectionCatalog::CollectionInfoFn callback,
                             CollectionCatalog::CollectionInfoFn predicate) {

    auto catalogForIteration = CollectionCatalog::get(opCtx);
    for (auto&& coll : catalogForIteration->range(dbName)) {
        auto uuid = coll->uuid();
        if (predicate && !catalogForIteration->checkIfCollectionSatisfiable(uuid, predicate)) {
            continue;
        }

        boost::optional<Lock::CollectionLock> clk;
        CollectionPtr collection;

        auto catalog = CollectionCatalog::get(opCtx);
        while (auto nss = catalog->lookupNSSByUUID(opCtx, uuid)) {
            // Get a fresh snapshot for each locked collection to see any catalog changes.
            clk.emplace(opCtx, *nss, collLockMode);
            opCtx->recoveryUnit()->abandonSnapshot();
            catalog = CollectionCatalog::get(opCtx);

            if (catalog->lookupNSSByUUID(opCtx, uuid) == nss) {
                // Success: locked the namespace and the UUID still maps to it.
                collection = CollectionPtr(catalog->lookupCollectionByUUID(opCtx, uuid));
                invariant(collection);
                break;
            }
            // Failed: collection got renamed before locking it, so unlock and try again.
            clk.reset();
        }

        // The NamespaceString couldn't be resolved from the uuid, so the collection was dropped.
        if (!collection)
            continue;

        if (!callback(collection.get()))
            break;

        hangBeforeGettingNextCollection.pauseWhileSet();
    }
}

}  // namespace catalog
}  // namespace mongo
