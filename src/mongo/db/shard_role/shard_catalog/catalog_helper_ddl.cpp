// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
