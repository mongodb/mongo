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

#include "mongo/db/s/drop_agg_temp_collections.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/router_role.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {

void dropTempCollection(OperationContext* opCtx, const NamespaceString& nss) {
    sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());
    router.route(opCtx,
                 "dropAggTempCollections",
                 [&nss](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                     // Drop the collection
                     const auto shard = uassertStatusOK(
                         Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cdb->getPrimary()));

                     ShardsvrDropCollection shardsvrDropCollection(nss);
                     generic_argument_util::setMajorityWriteConcern(shardsvrDropCollection);
                     const auto cmdResponse = executeCommandAgainstDatabasePrimary(
                         opCtx,
                         nss.dbName(),
                         cdb,
                         shardsvrDropCollection.toBSON(),
                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                         Shard::RetryPolicy::kIdempotent);

                     const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                     uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
                 });
}

}  // namespace

void dropAggTempCollections(OperationContext* opCtx) {
    // Before exiting drain mode, snapshot the list of temp collections to be deleted.
    DBDirectClient client(opCtx);
    const FindCommandRequest findRequest{NamespaceString::kAggTempCollections};
    auto cursor = client.find(findRequest);
    std::vector<NamespaceString> tempCollectionsToDrop;
    while (cursor->more()) {
        const auto doc = cursor->nextSafe().getOwned();
        NamespaceString nss = NamespaceStringUtil::deserialize(
            boost::none, doc.getField("_id").String(), SerializationContext::stateDefault());
        tempCollectionsToDrop.emplace_back(std::move(nss));
    }

    const auto serviceContext = opCtx->getServiceContext();
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    ExecutorFuture<void>(executor)
        .then([serviceContext, tempCollectionsToDrop] {
            for (const auto& nss : tempCollectionsToDrop) {
                ThreadClient tc{"dropAggTempCollections",
                                serviceContext->getService(ClusterRole::ShardServer)};
                const auto opCtx = tc->makeOperationContext();

                try {
                    dropTempCollection(opCtx.get(), nss);
                } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // The database might have been dropped by a different operation, so the temp
                    // collection does no longer exist.
                } catch (const DBException& ex) {
                    LOGV2(8144400,
                          "Failed to drop temporary aggregation collection",
                          logAttrs(nss),
                          "error"_attr = redact(ex.toString()));
                    // Do not remove the temporary collection entry.
                    continue;
                }

                // If the collection was successfully dropped, remove its temporary collection
                // entry.
                DBDirectClient client(opCtx.get());
                client.remove(NamespaceString::kAggTempCollections,
                              BSON("_id" << NamespaceStringUtil::serialize(
                                       nss, SerializationContext::stateDefault())));
            }
        })
        .getAsync([](auto) {});
}

}  // namespace mongo
