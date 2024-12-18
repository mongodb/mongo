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

#include "mongo/db/query/search/search_index_process_shard.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include <boost/optional/optional.hpp>


namespace mongo {

ServiceContext::ConstructorActionRegisterer SearchIndexProcessShardImplementation{
    "SearchIndexProcessShard-registration", [](ServiceContext* serviceContext) {
        invariant(serviceContext);
        // Only register the router implementation if this server has a shard service.
        if (auto service = serviceContext->getService(ClusterRole::ShardServer); service) {
            SearchIndexProcessInterface::set(service, std::make_unique<SearchIndexProcessShard>());
        }
    }};

boost::optional<UUID> SearchIndexProcessShard::fetchCollectionUUID(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    return CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
}

UUID SearchIndexProcessShard::fetchCollectionUUIDOrThrow(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    auto optUuid = fetchCollectionUUID(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection '" << nss.toStringForErrorMsg() << "' does not exist.",
            optUuid);
    return optUuid.get();
}

std::pair<boost::optional<UUID>, boost::optional<NamespaceString>>
SearchIndexProcessShard::fetchCollectionUUIDAndResolveView(OperationContext* opCtx,
                                                           const NamespaceString& nss) {

    auto catalog = CollectionCatalog::get(opCtx);
    auto view = catalog->lookupView(opCtx, nss);
    if (!view) {
        return std::make_pair(catalog->lookupUUIDByNSS(opCtx, nss), boost::none);
    } else {
        auto resolvedNamespace =
            view_catalog_helpers::findSourceCollectionNamespace(opCtx, catalog, nss);

        return std::make_pair(catalog->lookupUUIDByNSS(opCtx, resolvedNamespace),
                              boost::make_optional(resolvedNamespace));
    }
}

std::pair<UUID, boost::optional<NamespaceString>>
SearchIndexProcessShard::fetchCollectionUUIDAndResolveViewOrThrow(OperationContext* opCtx,
                                                                  const NamespaceString& nss) {
    auto uuidResolvdNssPair = fetchCollectionUUIDAndResolveView(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection '" << nss.toStringForErrorMsg() << "' does not exist.",
            uuidResolvdNssPair.first);

    return std::make_pair(*uuidResolvdNssPair.first, uuidResolvdNssPair.second);
}

}  // namespace mongo
