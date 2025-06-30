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


#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"
#include "mongo/s/router_role.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace cluster {
namespace {

MONGO_FAIL_POINT_DEFINE(createUnshardedCollectionRandomizeDataShard);

std::vector<AsyncRequestsSender::Request> buildUnshardedRequestsForAllShards(
    OperationContext* opCtx, std::vector<ShardId> shardIds, const BSONObj& cmdObj) {
    auto cmdToSend = cmdObj;
    appendShardVersion(cmdToSend, ShardVersion::UNSHARDED());

    std::vector<AsyncRequestsSender::Request> requests;
    requests.reserve(shardIds.size());
    for (auto&& shardId : shardIds)
        requests.emplace_back(std::move(shardId), cmdToSend);

    return requests;
}

AsyncRequestsSender::Response executeCommandAgainstFirstShard(OperationContext* opCtx,
                                                              const DatabaseName& dbName,
                                                              const CachedDatabaseInfo& dbInfo,
                                                              const BSONObj& cmdObj,
                                                              const ReadPreferenceSetting& readPref,
                                                              Shard::RetryPolicy retryPolicy) {
    auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    uassert(ErrorCodes::IllegalOperation, "there are no shards to target", !shardIds.empty());
    std::sort(shardIds.begin(), shardIds.end());
    ShardId shardId = shardIds[0];

    auto responses =
        gatherResponses(opCtx,
                        dbName,
                        readPref,
                        retryPolicy,
                        buildUnshardedRequestsForAllShards(
                            opCtx, {shardId}, appendDbVersionIfPresent(cmdObj, dbInfo)));
    return std::move(responses.front());
}

}  // namespace

CachedDatabaseInfo createDatabase(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  const boost::optional<ShardId>& suggestedPrimaryId) {
    auto catalogCache = Grid::get(opCtx)->catalogCache();

    auto dbStatus = catalogCache->getDatabase(opCtx, dbName);

    if (dbStatus == ErrorCodes::NamespaceNotFound) {
        ConfigsvrCreateDatabase request(
            DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest()));
        request.setDbName(DatabaseName::kAdmin);
        if (suggestedPrimaryId)
            request.setPrimaryShardId(*suggestedPrimaryId);

        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            txnRouter.annotateCreatedDatabase(dbName);
        }

        // If this is a database creation triggered by a command running inside a transaction, the
        // _configsvrCreateDatabase command here will also need to run inside that session. Yield
        // the session here. Otherwise, if this router is also the configsvr primary, the
        // _configsvrCreateDatabase command would not be able to check out the session.
        auto txnRouterResourceYielder = TransactionRouterResourceYielder::makeForRemoteCommand();
        auto sendCommand = [&] {
            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto response = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                DatabaseName::kAdmin,
                CommandHelpers::appendMajorityWriteConcern(request.toBSON({})),
                Shard::RetryPolicy::kIdempotent));
            return response;
        };
        auto response = runWithYielding(opCtx, txnRouterResourceYielder.get(), sendCommand);

        uassertStatusOK(response.writeConcernStatus);
        uassertStatusOKWithContext(response.commandStatus,
                                   str::stream() << "Database " << dbName.toStringForErrorMsg()
                                                 << " could not be created");

        auto createDbResponse = ConfigsvrCreateDatabaseResponse::parse(
            IDLParserContext("configsvrCreateDatabaseResponse"), response.response);
        catalogCache->onStaleDatabaseVersion(dbName, createDbResponse.getDatabaseVersion());

        dbStatus = catalogCache->getDatabase(opCtx, dbName);
    }

    return uassertStatusOK(std::move(dbStatus));
}

void createCollection(OperationContext* opCtx, const ShardsvrCreateCollection& request) {
    const auto& nss = request.getNamespace();
    const auto dbInfo = createDatabase(opCtx, nss.dbName());

    if (MONGO_unlikely(createUnshardedCollectionRandomizeDataShard.shouldFail()) &&
        request.getUnsplittable() && !request.getDataShard()) {
        // Select a random 'dataShard'.
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto allShardIds = shardRegistry->getAllShardIds(opCtx);
        std::random_device random_device;
        std::mt19937 engine{random_device()};
        std::uniform_int_distribution<int> dist(0, allShardIds.size() - 1);

        ShardsvrCreateCollection requestWithRandomDataShard(request);
        requestWithRandomDataShard.setDataShard(allShardIds[dist(engine)]);

        LOGV2_DEBUG(8339600,
                    2,
                    "Selected a random data shard for createCollection",
                    "nss"_attr = nss,
                    "dataShard"_attr = *requestWithRandomDataShard.getDataShard());

        try {
            createCollection(opCtx, requestWithRandomDataShard);
            return;
        } catch (const ExceptionFor<ErrorCodes::AlreadyInitialized>&) {
            // If the collection already exists but we randomly selected a dataShard that turns out
            // to be different than the current one, then createCollection will fail with
            // AlreadyInitialized error. However, this error can also occur for other reasons. So
            // let's run createCollection again without selecting a random dataShard.
        }
    }

    BSONObjBuilder builder;
    request.serialize({}, &builder);

    auto rc = repl::ReadConcernArgs::get(opCtx);
    rc.appendInfo(&builder);

    const bool isSharded = !request.getUnsplittable();
    auto cmdObjWithWc = [&]() {
        // TODO SERVER-77915: Remove the check "isSharded && nss.isConfigDB()" once 8.0 becomes last
        // LTS. This is a special check for config.system.sessions since the request comes from
        // the CSRS which is upgraded first
        if (isSharded && nss.isConfigDB()) {
            return CommandHelpers::appendMajorityWriteConcern(builder.obj());
        }
        // propagate write concern if asked by the caller otherwise we set
        //  - majority if we are not in a transaction
        //  - default wc in case of transaction (no other wc are allowed).
        if (opCtx->getWriteConcern().getProvenance().isClientSupplied()) {
            auto wc = opCtx->getWriteConcern();
            return CommandHelpers::appendWCToObj(builder.obj(), wc);
        } else {
            if (opCtx->inMultiDocumentTransaction()) {
                return builder.obj();
            } else {
                // TODO SERVER-82859 remove appendMajorityWriteConcern
                return CommandHelpers::appendMajorityWriteConcern(builder.obj());
            }
        }
    }();
    auto cmdResponse = [&]() {
        if (isSharded && nss.isConfigDB())
            return executeCommandAgainstFirstShard(
                opCtx,
                nss.dbName(),
                dbInfo,
                cmdObjWithWc,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);
        else {
            return executeDDLCoordinatorCommandAgainstDatabasePrimary(
                opCtx,
                nss.dbName(),
                dbInfo,
                cmdObjWithWc,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);
        }
    }();

    const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
    uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));

    auto createCollResp =
        CreateCollectionResponse::parse(IDLParserContext("createCollection"), remoteResponse.data);

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        nss, createCollResp.getCollectionVersion(), dbInfo->getPrimary());
}

void createCollectionWithRouterLoop(OperationContext* opCtx,
                                    const ShardsvrCreateCollection& request) {
    sharding::router::CollectionRouter router(opCtx->getServiceContext(), request.getNamespace());
    router.route(opCtx,
                 "cluster::createCollectionWithRouterLoop",
                 [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                     cluster::createCollection(opCtx, request);
                 });
}

void createCollectionWithRouterLoop(OperationContext* opCtx, const NamespaceString& nss) {
    auto dbName = nss.dbName();

    ShardsvrCreateCollection shardsvrCollCommand(nss);
    ShardsvrCreateCollectionRequest request;

    request.setUnsplittable(true);

    shardsvrCollCommand.setShardsvrCreateCollectionRequest(request);
    shardsvrCollCommand.setDbName(nss.dbName());

    createCollectionWithRouterLoop(opCtx, shardsvrCollCommand);
}

}  // namespace cluster
}  // namespace mongo
