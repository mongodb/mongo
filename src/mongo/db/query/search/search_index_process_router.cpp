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

boost::optional<UUID> SearchIndexProcessRouter::fetchCollectionUUID(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    // We perform a listCollection request to get the UUID from the actual primary shard for the
    // database. This will ensure it is correct for both SHARDED and UNSHARDED versions of the
    // collection.
    sharding::router::DBPrimaryRouter router{opCtx->getServiceContext(), nss.dbName()};

    auto uuid = router.route(
        opCtx,
        "get collection UUID",
        [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) -> boost::optional<UUID> {
            ListCollections listCollections;
            listCollections.setDbName(nss.dbName());
            listCollections.setFilter(BSON("name" << nss.coll()));

            auto response = executeCommandAgainstDatabasePrimary(
                opCtx,
                nss.dbName(),
                cdb,
                listCollections.toBSON(),
                ReadPreferenceSetting(ReadPreference::PrimaryPreferred),
                Shard::RetryPolicy::kIdempotent);

            // We consider an empty response to mean that the collection doesn't exist.
            auto batch = uassertStatusOK(response.swResponse).data["cursor"]["firstBatch"].Array();
            if (batch.empty()) {
                return boost::none;
            }
            const auto& bsonDoc = batch.front();
            auto uuid = UUID::parse(bsonDoc["info"]["uuid"]);
            if (!uuid.isOK()) {
                return boost::none;
            }
            return uuid.getValue();
        });
    return uuid;
}

UUID SearchIndexProcessRouter::fetchCollectionUUIDOrThrow(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    auto uuid = fetchCollectionUUID(opCtx, nss);
    if (!uuid) {
        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "collection " << nss.toStringForErrorMsg() << " does not exist");
    }
    return *uuid;
}

}  // namespace mongo
