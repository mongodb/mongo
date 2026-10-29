// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/search/search_index_process_shard.h"

#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/views/view_catalog_helpers.h"

#include <boost/optional/optional.hpp>


namespace mongo {

ServiceContext::ConstructorActionRegisterer SearchIndexProcessShardImplementation{
    "SearchIndexProcessShard-registration", [](ServiceContext* serviceContext) {
        invariant(serviceContext);
        // Only register the router implementation if this server has a shard service.
        if (auto service = serviceContext->getService();
            service->role().has(ClusterRole::ShardServer)) {
            SearchIndexProcessInterface::set(service, std::make_unique<SearchIndexProcessShard>());
        }
    }};


std::pair<boost::optional<UUID>, boost::optional<ResolvedNamespace>>
SearchIndexProcessShard::fetchCollectionUUIDAndResolveView(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           bool failOnTsColl) {
    auto catalog = CollectionCatalog::get(opCtx);  // NOLINT TODO: SERVER-104335 Remove this.
    auto coll = catalog->lookupCollectionByNamespace(opCtx, nss);
    auto view = catalog->lookupView(opCtx, nss);

    uassert(10840701,
            "search index commands are not allowed on timeseries collections",
            !(failOnTsColl &&
              ((coll && coll->isTimeseriesCollection()) || (view && view->timeseries()))));

    if (!view) {
        return std::make_pair(catalog->lookupUUIDByNSS(opCtx, nss), boost::none);
    } else {
        auto resolvedView =
            view_catalog_helpers::resolveView(opCtx, catalog, nss, boost::none).getValue();

        return std::make_pair(catalog->lookupUUIDByNSS(opCtx, resolvedView.getResolvedNamespace()),
                              boost::make_optional(resolvedView));
    }
}

std::pair<UUID, boost::optional<ResolvedNamespace>>
SearchIndexProcessShard::fetchCollectionUUIDAndResolveViewOrThrow(OperationContext* opCtx,
                                                                  const NamespaceString& nss) {
    auto uuidResolvedViewPair = fetchCollectionUUIDAndResolveView(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection '" << nss.toStringForErrorMsg() << "' does not exist.",
            uuidResolvedViewPair.first);

    return std::make_pair(*uuidResolvedViewPair.first, uuidResolvedViewPair.second);
}

}  // namespace mongo
