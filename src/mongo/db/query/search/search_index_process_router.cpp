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

#include "mongo/db/list_collections_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/router_role.h"

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
StatusWith<std::pair<boost::optional<UUID>, boost::optional<NamespaceString>>> resolveViewHelper(
    OperationContext* opCtx,
    const CachedDatabaseInfo& cdb,
    ListCollections listCollections,
    StringData sourceCollection,
    NamespaceString nss) {
    int depth = 0;

    std::string viewOn;
    for (; depth < ViewGraph::kMaxViewDepth; depth++) {
        listCollections.setFilter(BSON("name" << sourceCollection));
        auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
            opCtx,
            nss.dbName(),
            cdb,
            listCollections.toBSON(),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);
        auto batch = uassertStatusOK(response.swResponse).data["cursor"]["firstBatch"].Array();

        if (batch.empty()) {
            return std::make_pair(boost::none, boost::none);
        }
        const auto& bsonDoc = batch.front();

        if (bsonDoc["type"].String() != "view") {
            auto uuid = UUID::parse(bsonDoc["info"]["uuid"]);
            if (!uuid.isOK()) {
                return std::make_pair(boost::none, boost::none);
            }
            return std::make_pair(uuid.getValue(),
                                  boost::make_optional(NamespaceStringUtil::deserialize(
                                      nss.dbName(), sourceCollection)));
        }
        viewOn = bsonDoc["options"]["viewOn"].String();
        sourceCollection = StringData(viewOn.c_str(), viewOn.size());
    }
    if (depth >= ViewGraph::kMaxViewDepth) {
        return {ErrorCodes::ViewDepthLimitExceeded,
                str::stream() << "View depth too deep or view cycle detected; maximum depth is "
                              << ViewGraph::kMaxViewDepth};
    }

    MONGO_UNREACHABLE;
}
}  // namespace

std::pair<boost::optional<UUID>, boost::optional<NamespaceString>>
SearchIndexProcessRouter::fetchCollectionUUIDAndResolveView(OperationContext* opCtx,
                                                            const NamespaceString& nss) {
    sharding::router::DBPrimaryRouter router{opCtx->getServiceContext(), nss.dbName()};

    auto uuidAndPossibleCollName = router.route(
        opCtx,
        "get collection UUID",
        [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb)
            -> std::pair<boost::optional<UUID>, boost::optional<NamespaceString>> {
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

            if (bsonDoc["type"].String() == "view") {
                auto sourceCollection = bsonDoc["options"]["viewOn"].String();
                auto status = resolveViewHelper(opCtx, cdb, listCollections, sourceCollection, nss);
                return uassertStatusOK(status);
            }

            auto uuid = UUID::parse(bsonDoc["info"]["uuid"]);
            if (!uuid.isOK()) {
                return std::make_pair(boost::none, boost::none);
            }
            // normal collection
            return std::make_pair(boost::make_optional(uuid.getValue()), boost::none);
        });
    return uuidAndPossibleCollName;
}

std::pair<UUID, boost::optional<NamespaceString>>
SearchIndexProcessRouter::fetchCollectionUUIDAndResolveViewOrThrow(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    auto uuidResolvdNssPair = fetchCollectionUUIDAndResolveView(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection '" << nss.toStringForErrorMsg() << "' does not exist.",
            uuidResolvdNssPair.first);

    return std::make_pair(*uuidResolvdNssPair.first, uuidResolvdNssPair.second);
}

}  // namespace mongo
