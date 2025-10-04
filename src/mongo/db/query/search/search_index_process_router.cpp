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

#include "mongo/db/query/search/search_index_process_router.h"

#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/views/view_graph.h"

#include <boost/optional/optional.hpp>

namespace mongo {

ServiceContext::ConstructorActionRegisterer SearchIndexProcessRouterImplementation{
    "SearchIndexProcessRouter-registration", [](ServiceContext* serviceContext) {
        invariant(serviceContext);
        // Only register the router implementation if this server has a router service.
        if (auto service = serviceContext->getService(ClusterRole::RouterServer); service) {
            SearchIndexProcessInterface::set(service, std::make_unique<SearchIndexProcessRouter>());
        }
    }};

namespace {
// Currently, the views catalog lives on the primary shard. However, in Atlas search, the router
// handles sharded search index commands. Therefore to support search index commands on sharded
// views, the router must descend the view graph by recursively calling listCollections on the
// primary shard until we reach the storage collection. The risk in this approach is that the views
// catalog might change in between each listCollections invocation (eg the user drops one of the
// ancestor views, modifies an ancestor's view definition, etc). Though not ideal, the current work
// solution is that if a user changes the view graph and invalidates their correspondent mongot
// index, said index will be silently killed and queries using that index will return no results.
// When future work is completed to cache the views catalog on the router aware, we will be able to
// eliminate this race condition and get a single/locked instance of the view graph for each search
// index command.
StatusWith<std::pair<boost::optional<UUID>, boost::optional<ResolvedView>>> resolveViewHelper(
    OperationContext* opCtx, const CachedDatabaseInfo& cdb, NamespaceString nss) {

    BSONObjBuilder bob;
    bob.append("_shardsvrResolveView", 1);
    bob.append("nss", NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));

    auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
        opCtx,
        nss.dbName(),
        cdb,
        bob.obj(),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kIdempotent);
    boost::optional<ResolvedView> resolvedView;
    boost::optional<UUID> uuid;
    auto data = uassertStatusOK(response.swResponse).data;

    if (data.hasField("resolvedView")) {
        resolvedView = boost::make_optional(ResolvedView::parseFromBSON(data["resolvedView"]));
    }
    if (data.hasField("collectionUUID")) {
        uuid = boost::make_optional(uassertStatusOK(UUID::parse(data["collectionUUID"])));
    }
    return std::make_pair(uuid, resolvedView);
}
}  // namespace

std::pair<boost::optional<UUID>, boost::optional<ResolvedView>>
SearchIndexProcessRouter::fetchCollectionUUIDAndResolveView(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            bool failOnTsColl) {
    sharding::router::DBPrimaryRouter router{opCtx->getServiceContext(), nss.dbName()};

    auto uuidAndPossibleCollName = router.route(
        opCtx,
        "get collection UUID",
        [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb)
            -> std::pair<boost::optional<UUID>, boost::optional<ResolvedView>> {
            ListCollections listCollections;
            listCollections.setDbName(nss.dbName());
            listCollections.setFilter(BSON("name" << nss.coll()));

            auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                opCtx,
                nss.dbName(),
                cdb,
                listCollections.toBSON(),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);

            // We consider an empty response to mean that the collection doesn't exist.
            auto batch = uassertStatusOK(response.swResponse).data["cursor"]["firstBatch"].Array();
            if (batch.empty()) {
                return std::make_pair(boost::none, boost::none);
            }
            const auto& bsonDoc = batch.front();

            // Search index related commands should fail when attempted on a ts collection.
            // Except '$listSearchIndexes' should return empty results.
            // TODO SERVER-110353: Investigate why "type" returns "collection" instead of
            // "timeseries" when:
            // - Running $listSearchIndexes (but not create/update/dropSearchIndex).
            // - On a sharded collection (but not an unsharded collection on a sharded cluster).
            // - Against a viewless timeseries collection (but not a viewfull ts collection).
            uassert(10840700,
                    "search index commands are not allowed on timeseries collections",
                    !(failOnTsColl && bsonDoc["type"].String() == "timeseries"));

            if (bsonDoc["type"].String() == "view") {
                auto sourceCollection = bsonDoc["options"]["viewOn"].String();
                auto status = resolveViewHelper(opCtx, cdb, nss);
                return uassertStatusOK(status);
            }

            auto uuid = UUID::parse(bsonDoc["info"]["uuid"]);
            if (!uuid.isOK()) {
                return std::make_pair(boost::none, boost::none);
            }
            // The search index command is being ran on a normal source collection eg not a view.
            return std::make_pair(boost::make_optional(uuid.getValue()), boost::none);
        });
    return uuidAndPossibleCollName;
}

std::pair<UUID, boost::optional<ResolvedView>>
SearchIndexProcessRouter::fetchCollectionUUIDAndResolveViewOrThrow(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    auto uuidResolvdNssPair = fetchCollectionUUIDAndResolveView(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection '" << nss.toStringForErrorMsg() << "' does not exist.",
            uuidResolvdNssPair.first);

    return std::make_pair(*uuidResolvdNssPair.first, uuidResolvdNssPair.second);
}

}  // namespace mongo
