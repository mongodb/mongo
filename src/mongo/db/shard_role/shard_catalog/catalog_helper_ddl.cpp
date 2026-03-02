/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/shard_role/shard_catalog/catalog_helper_ddl.h"

#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
boost::optional<NamespaceString> extractViewNss(const CollectionOrViewAcquisitions& acquisitions) {
    for (const auto& acq : acquisitions) {
        if (acq.isView()) {
            return acq.nss();
        }
    }
    return boost::none;
}

catalog_helper_ddl::AcquisitionsForCatalogWrites reacquireCollectionsWithSystemViews(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const CollectionOrViewAcquisitionRequests& requests) {
    auto systemViewsNss = NamespaceString::makeSystemDotViewsNamespace(dbName);
    auto systemViewsRequest = CollectionOrViewAcquisitionRequest::fromOpCtx(
        opCtx, systemViewsNss, AcquisitionPrerequisites::kWrite);
    auto newRequests = requests;
    newRequests.emplace_back(systemViewsRequest);

    auto collectionOrViewAcquisitions = acquireCollectionsOrViews(opCtx, newRequests, MODE_X);
    auto viewNss2 = extractViewNss(collectionOrViewAcquisitions);
    // Confirm we still have a view
    if (viewNss2) {
        return catalog_helper_ddl::AcquisitionsForCatalogWrites(
            makeAcquisitionMap(collectionOrViewAcquisitions));
    } else {
        // Given that what we thought was a view now is not present or became a collection, drop the
        // systemView acquisition.
        auto map = makeAcquisitionMap(collectionOrViewAcquisitions);
        map.erase(systemViewsNss);
        return catalog_helper_ddl::AcquisitionsForCatalogWrites(map);
    }
}

}  // namespace

namespace catalog_helper_ddl {

AcquisitionsForCatalogWrites acquireCollectionOrViewForCatalogWrites(
    OperationContext* opCtx, const CollectionOrViewAcquisitionRequests& requests) {

    // In case of a view, we need to acquire the system.views as well.
    // Given the shard role api acquisition opens a snapshot, we can't just add a new
    // acquisition here because it would be based on the same snapshot as the view, which could
    // be stale. DDLs preform a MODE_X acquisition so that they are certain to have access to
    // the latest version of all the nss they require. Note this is a workaround to the fact
    // that the shard_role_api acquisition opens a snapshot.
    boost::optional<NamespaceString> viewNss;
    {
        auto collectionOrViewAcquisitions = acquireCollectionsOrViews(opCtx, requests, MODE_X);
        viewNss = extractViewNss(collectionOrViewAcquisitions);
        if (!viewNss) {

            return AcquisitionsForCatalogWrites(makeAcquisitionMap(collectionOrViewAcquisitions));
        }
    }
    return reacquireCollectionsWithSystemViews(opCtx, viewNss->dbName(), requests);
}

}  // namespace catalog_helper_ddl
}  // namespace mongo
