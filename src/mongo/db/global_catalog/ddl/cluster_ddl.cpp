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


#include "mongo/db/global_catalog/ddl/cluster_ddl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace cluster {
namespace {

MONGO_FAIL_POINT_DEFINE(createUnshardedCollectionRandomizeDataShard);
MONGO_FAIL_POINT_DEFINE(hangCreateUnshardedCollection);

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

// TODO (SERVER-100309): remove once 9.0 becomes last LTS.
AsyncRequestsSender::Response executeCommandAgainstFirstShard(OperationContext* opCtx,
                                                              const DatabaseName& dbName,
                                                              const NamespaceString& nss,
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
                        nss,
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
        generic_argument_util::setMajorityWriteConcern(request);
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
            auto response = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        request.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));
            return response;
        };
        auto response = runWithYielding(opCtx, txnRouterResourceYielder.get(), sendCommand);
        uassertStatusOK(response.writeConcernStatus);
        uassertStatusOKWithContext(response.commandStatus,
                                   str::stream() << "Database " << dbName.toStringForErrorMsg()
                                                 << " could not be created");

        auto createDbResponse = ConfigsvrCreateDatabaseResponse::parse(
            response.response, IDLParserContext("configsvrCreateDatabaseResponse"));
        catalogCache->onStaleDatabaseVersion(dbName, createDbResponse.getDatabaseVersion());

        dbStatus = catalogCache->getDatabase(opCtx, dbName);
    }

    return uassertStatusOK(std::move(dbStatus));
}

CreateCollectionResponse createCollection(OperationContext* opCtx,
                                          ShardsvrCreateCollection request,
                                          bool againstFirstShard) {
    const auto& nss = request.getNamespace();

    if (MONGO_unlikely(hangCreateUnshardedCollection.shouldFail()) && request.getUnsplittable() &&
        request.getDataShard() && request.getIsFromCreateUnsplittableCollectionTestCommand()) {
        LOGV2(9913801, "Hanging createCollection due to failpoint 'hangCreateUnshardedCollection'");
        hangCreateUnshardedCollection.pauseWhileSet();
        LOGV2(9913802,
              "Hanging createCollection due to failpoint 'hangCreateUnshardedCollection' finished");
    }

    const auto dbInfo = createDatabase(opCtx, nss.dbName());

    // The config.system.session collection can only exist as sharded and it's essential for the
    // correct cluster functionality. To prevent potential issues, the operation must always be
    // performed as a sharded creation with internal defaults. Ignore potentially different
    // user-provided parameters.
    if (nss == NamespaceString::kLogicalSessionsNamespace) {
        auto newRequest = shardLogicalSessionsCollectionRequest();
        bool isValidRequest = newRequest.getShardsvrCreateCollectionRequest().toBSON().woCompare(
                                  request.getShardsvrCreateCollectionRequest().toBSON()) == 0;
        if (!isValidRequest) {
            LOGV2_WARNING(
                9733600,
                "Detected an invalid creation request for config.system.sessions. To guarantee "
                "the correct "
                "behavior, the request will be replaced with a shardCollection with internal "
                "defaults",
                "nss"_attr = nss.toStringForErrorMsg(),
                "original request"_attr = request.toBSON(),
                "new request"_attr = newRequest.toBSON());
            request = newRequest;
        }
    }

    if (MONGO_unlikely(createUnshardedCollectionRandomizeDataShard.shouldFail()) &&
        request.getUnsplittable() && !request.getDataShard() &&
        !request.getRegisterExistingCollectionInGlobalCatalog()) {
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
            return createCollection(opCtx, std::move(requestWithRandomDataShard));
        } catch (const ExceptionFor<ErrorCodes::AlreadyInitialized>&) {
            // If the collection already exists but we randomly selected a dataShard that turns out
            // to be different than the current one, then createCollection will fail with
            // AlreadyInitialized error. However, this error can also occur for other reasons. So
            // let's run createCollection again without selecting a random dataShard.
        } catch (const ExceptionFor<ErrorCodes::ShardNotFound>&) {
            // It's possible that the dataShard no longer exists. For example, the config shard
            // may have been migrated to a dedicated config server during the test.
            // In this case, the original request will be used to create the collection.
        }
    }
    // Note that this check must run separately to manage the case a request already comes with
    // dataShard selected (as for $out or specific tests) and the checkpoint is enabled. In that
    // case, we leave the dataShard selected instead of randomize it, but we will track the
    // collection.
    if (MONGO_unlikely(createUnshardedCollectionRandomizeDataShard.shouldFail()) &&
        request.getDataShard()) {
        // To set dataShard we need to pass from the coordinator and track the collection. Only the
        // createUnsplittableCollection command can do that. Convert the create request to the test
        // command request by enabling the flag below when the failpoint is active.
        request.setIsFromCreateUnsplittableCollectionTestCommand(true);
    }

    request.setReadConcern(repl::ReadConcernArgs::get(opCtx));

    const bool isSharded = !request.getUnsplittable();
    auto cmdObjWithWc = [&]() {
        // TODO SERVER-77915: Remove the check "isSharded && nss.isConfigDB()" once 8.0 becomes last
        // LTS. This is a special check for config.system.sessions since the request comes from
        // the CSRS which is upgraded first
        if (isSharded && nss.isConfigDB()) {
            generic_argument_util::setMajorityWriteConcern(request);
            return request.toBSON();
        }

        // Upgrade the request WC to 'majority', unless it is part of a transaction
        // (where only the implicit default value can be applied).
        if (!opCtx->inMultiDocumentTransaction()) {
            generic_argument_util::setMajorityWriteConcern(request);
        }
        return request.toBSON();
    }();

    boost::optional<executor::RemoteCommandResponse> remoteResponse;

    // TODO (SERVER-100309): remove againstFirstShard option once 9.0 becomes last LTS.
    if (againstFirstShard) {
        const auto cmdResponse =
            executeCommandAgainstFirstShard(opCtx,
                                            nss.dbName(),
                                            nss,
                                            dbInfo,
                                            cmdObjWithWc,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            Shard::RetryPolicy::kIdempotent);
        remoteResponse = uassertStatusOK(cmdResponse.swResponse);
        uassertStatusOK(getStatusFromCommandResult(remoteResponse->data));
        uassertStatusOK(getWriteConcernStatusFromCommandResult(remoteResponse->data));

    } else {
        sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());
        router.route(
            opCtx,
            "createCollection"_sd,
            [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                const auto cmdResponse = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                    opCtx,
                    nss.dbName(),
                    dbInfo,
                    cmdObjWithWc,
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    Shard::RetryPolicy::kIdempotent);

                remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                uassertStatusOK(getStatusFromCommandResult(remoteResponse->data));
                uassertStatusOK(getWriteConcernStatusFromCommandResult(remoteResponse->data));
            });
    }
    auto createCollResp =
        CreateCollectionResponse::parse(remoteResponse->data, IDLParserContext("createCollection"));

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    catalogCache->onStaleCollectionVersion(nss, createCollResp.getCollectionVersion());

    return createCollResp;
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

ShardsvrCreateCollection shardLogicalSessionsCollectionRequest() {
    ShardsvrCreateCollection systemSessionRequest(NamespaceString::kLogicalSessionsNamespace);
    ShardsvrCreateCollectionRequest params;
    params.setShardKey(BSON("_id" << 1));
    systemSessionRequest.setShardsvrCreateCollectionRequest(std::move(params));
    systemSessionRequest.setDbName(NamespaceString::kLogicalSessionsNamespace.dbName());
    return systemSessionRequest;
}

}  // namespace cluster
}  // namespace mongo
