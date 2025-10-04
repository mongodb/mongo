/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/index_on_config.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_config_version_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/local_catalog/coll_mod.h"
#include "mongo/db/local_catalog/collection_options_gen.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <tuple>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(shardingCatalogManagerWithTransactionFailWCAfterCommit);
MONGO_FAIL_POINT_DEFINE(shardingCatalogManagerSkipNotifyClusterOnNewDatabases);

const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

// This value is initialized only if the node is running as a config server
const auto getShardingCatalogManager =
    ServiceContext::declareDecoration<boost::optional<ShardingCatalogManager>>();

OpMsg runCommandInLocalTxn(OperationContext* opCtx,
                           const DatabaseName& db,
                           bool startTransaction,
                           TxnNumber txnNumber,
                           BSONObj cmdObj) {
    BSONObjBuilder bob(std::move(cmdObj));
    if (startTransaction) {
        bob.append("startTransaction", true);
    }
    bob.append("autocommit", false);
    bob.append(OperationSessionInfoFromClient::kTxnNumberFieldName, txnNumber);

    BSONObjBuilder lsidBuilder(bob.subobjStart("lsid"));
    opCtx->getLogicalSessionId()->serialize(&bob);
    lsidBuilder.doneFast();

    return OpMsg::parseOwned(
        opCtx->getService()
            ->getServiceEntryPoint()
            ->handleRequest(
                opCtx,
                OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx), db, bob.obj())
                    .serialize(),
                opCtx->fastClockSource().now())
            .get()
            .response);
}

/**
 * Runs the BatchedCommandRequest 'request' on namespace 'nss' It transforms the request to BSON
 * and then uses a DBDirectClient to run the command locally.
 */
BSONObj executeConfigRequest(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BatchedCommandRequest& request) {
    invariant(nss.dbName() == DatabaseName::kConfig);
    DBDirectClient client(opCtx);
    BSONObj result;
    client.runCommand(nss.dbName(), request.toBSON(), result);
    return result;
}

void startTransactionWithNoopFind(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  TxnNumber txnNumber) {
    FindCommandRequest findCommand(nss);
    findCommand.setBatchSize(0);
    findCommand.setSingleBatch(true);

    auto res = runCommandInLocalTxn(
                   opCtx, nss.dbName(), true /*startTransaction*/, txnNumber, findCommand.toBSON())
                   .body;
    uassertStatusOK(getStatusFromCommandResult(res));
}

BSONObj commitOrAbortTransaction(OperationContext* opCtx,
                                 TxnNumber txnNumber,
                                 std::string cmdName,
                                 const WriteConcernOptions& writeConcern) {
    // Swap out the clients in order to get a fresh opCtx. Previous operations in this transaction
    // that have been run on this opCtx would have set the timeout in the locker on the opCtx, but
    // commit should not have a lock timeout.
    auto newClient = getGlobalServiceContext()
                         ->getService(ClusterRole::ShardServer)
                         ->makeClient("ShardingCatalogManager");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    AuthorizationSession::get(newOpCtx.get()->getClient())->grantInternalAuthorization();
    {
        auto lk = stdx::lock_guard(*newOpCtx->getClient());
        newOpCtx->setLogicalSessionId(opCtx->getLogicalSessionId().value());
        newOpCtx->setTxnNumber(txnNumber);
    }

    BSONObjBuilder bob;
    bob.append(cmdName, true);
    bob.append("autocommit", false);
    bob.append(OperationSessionInfoFromClient::kTxnNumberFieldName, txnNumber);
    bob.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());

    BSONObjBuilder lsidBuilder(bob.subobjStart("lsid"));
    newOpCtx->getLogicalSessionId()->serialize(&bob);
    lsidBuilder.doneFast();

    const auto cmdObj = bob.obj();

    const auto replyOpMsg = OpMsg::parseOwned(
        newOpCtx->getService()
            ->getServiceEntryPoint()
            ->handleRequest(newOpCtx.get(),
                            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                                        DatabaseName::kAdmin,
                                                        cmdObj)
                                .serialize(),
                            newOpCtx->fastClockSource().now())
            .get()
            .response);
    return replyOpMsg.body;
}

// Runs commit for the transaction with 'txnNumber'.
auto commitTransaction(OperationContext* opCtx,
                       TxnNumber txnNumber,
                       const WriteConcernOptions& writeConcern) {
    auto response = commitOrAbortTransaction(opCtx, txnNumber, "commitTransaction", writeConcern);
    return std::make_tuple(getStatusFromCommandResult(response),
                           getWriteConcernStatusFromCommandResult(response));
}

// Runs abort for the transaction with 'txnNumber'.
void abortTransaction(OperationContext* opCtx,
                      TxnNumber txnNumber,
                      const WriteConcernOptions& writeConcern) {
    auto response = commitOrAbortTransaction(opCtx, txnNumber, "abortTransaction", writeConcern);

    // It is safe to ignore write concern errors in the presence of a NoSuchTransaction command
    // error because the transaction being aborted was both generated by and run locally on this
    // replica set primary. The NoSuchTransaction decision couldn't end up being rolled back.
    auto status = getStatusFromCommandResult(response);
    if (status.code() != ErrorCodes::NoSuchTransaction) {
        uassertStatusOK(status);
        uassertStatusOK(getWriteConcernStatusFromCommandResult(response));
    }
}

Status createIndexesForConfigChunks(OperationContext* opCtx) {
    const bool unique = true;
    Status result = createIndexOnConfigCollection(
        opCtx,
        NamespaceString::kConfigsvrChunksNamespace,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_min_1 index on config.chunks");
    }

    result = createIndexOnConfigCollection(
        opCtx,
        NamespaceString::kConfigsvrChunksNamespace,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_shard_1_min_1 index on config.chunks");
    }

    result = createIndexOnConfigCollection(
        opCtx,
        NamespaceString::kConfigsvrChunksNamespace,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::lastmod() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_lastmod_1 index on config.chunks");
    }

    result = createIndexOnConfigCollection(opCtx,
                                           NamespaceString::kConfigsvrChunksNamespace,
                                           BSON(ChunkType::collectionUUID()
                                                << 1 << ChunkType::shard() << 1
                                                << ChunkType::onCurrentShardSince() << 1),
                                           false /* unique */);
    if (!result.isOK()) {
        return result.withContext(
            "couldn't create uuid_1_shard_1_onCurrentShardSince_1 index on config.chunks");
    }

    return Status::OK();
}

// creates a vector of a vector of BSONObj (one for each batch) from the docs vector
// each batch can only be as big as the maximum BSON Object size and be below the maximum
// document count
std::vector<std::vector<BSONObj>> createBulkWriteBatches(const std::vector<BSONObj>& docs,
                                                         int documentOverhead) {

    const auto maxBatchSize = write_ops::kMaxWriteBatchSize;

    // creates a vector of a vector of BSONObj (one for each batch) from the docs vector
    // each batch can only be as big as the maximum BSON Object size and be below the maximum
    // document count
    std::vector<std::vector<BSONObj>> out;
    size_t batchIndex = 0;
    int workingBatchDocSize = 0;

    std::for_each(docs.begin(), docs.end(), [&](const BSONObj& doc) {
        if (out.size() == batchIndex) {
            out.emplace_back(std::vector<BSONObj>());
        }

        auto currentBatchBSONSize = workingBatchDocSize + doc.objsize() + documentOverhead;

        if (currentBatchBSONSize > BSONObjMaxUserSize ||
            out[batchIndex].size() + 1 > maxBatchSize) {
            ++batchIndex;
            workingBatchDocSize = 0;
            out.emplace_back(std::vector<BSONObj>());
        }
        out[batchIndex].emplace_back(doc);
        workingBatchDocSize += doc.objsize() + documentOverhead;
    });

    return out;
};
}  // namespace

void ShardingCatalogManager::create(ServiceContext* serviceContext,
                                    std::shared_ptr<executor::TaskExecutor> addShardExecutor,
                                    std::shared_ptr<Shard> localConfigShard,
                                    std::unique_ptr<ShardingCatalogClient> localCatalogClient) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(!shardingCatalogManager);

    shardingCatalogManager.emplace(serviceContext,
                                   std::move(addShardExecutor),
                                   std::move(localConfigShard),
                                   std::move(localCatalogClient));
}

void ShardingCatalogManager::clearForTests(ServiceContext* serviceContext) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(shardingCatalogManager);

    shardingCatalogManager.reset();
}

ShardingCatalogManager* ShardingCatalogManager::get(ServiceContext* serviceContext) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(shardingCatalogManager);

    return shardingCatalogManager.get_ptr();
}

ShardingCatalogManager* ShardingCatalogManager::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

ShardingCatalogManager::ShardingCatalogManager(
    ServiceContext* serviceContext,
    std::shared_ptr<executor::TaskExecutor> addShardExecutor,
    std::shared_ptr<Shard> localConfigShard,
    std::unique_ptr<ShardingCatalogClient> localCatalogClient)
    : _serviceContext(serviceContext),
      _executorForAddShard(std::move(addShardExecutor)),
      _localConfigShard(std::move(localConfigShard)),
      _localCatalogClient(std::move(localCatalogClient)),
      _kShardMembershipLock("shardMembershipLock"),
      _kClusterCardinalityParameterLock("clusterCardinalityParameterLock"),
      _kChunkOpLock("chunkOpLock"),
      _kZoneOpLock("zoneOpLock") {
    startup();
}

ShardingCatalogManager::~ShardingCatalogManager() {
    shutDown();
}

void ShardingCatalogManager::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_started) {
        return;
    }

    _started = true;
    _executorForAddShard->startup();

    Grid::get(_serviceContext)
        ->setCustomConnectionPoolStatsFn(
            [this](executor::ConnectionPoolStats* stats) { appendConnectionStats(stats); });
}

void ShardingCatalogManager::shutDown() {
    Grid::get(_serviceContext)->setCustomConnectionPoolStatsFn(nullptr);
    _executorForAddShard->shutdown();
    _executorForAddShard->join();
}

Status ShardingCatalogManager::initializeConfigDatabaseIfNeeded(OperationContext* opCtx) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_configInitialized) {
            return {ErrorCodes::AlreadyInitialized,
                    "Config database was previously loaded into memory"};
        }
    }

    Status status = _initConfigCollections(opCtx);
    if (!status.isOK()) {
        return status;
    }

    status = _initConfigIndexes(opCtx);
    if (!status.isOK()) {
        return status;
    }

    status = _initConfigSettings(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Make sure to write config.version last since we detect rollbacks of config.version and
    // will re-run initializeConfigDatabaseIfNeeded if that happens, but we don't detect rollback
    // of the index builds.
    status = _initConfigVersion(opCtx);
    if (!status.isOK()) {
        return status;
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configInitialized = true;

    return Status::OK();
}

ShardingCatalogClient* ShardingCatalogManager::localCatalogClient() {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    return _localCatalogClient.get();
}

const std::shared_ptr<Shard>& ShardingCatalogManager::localConfigShard() {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    return _localConfigShard;
}

void ShardingCatalogManager::discardCachedConfigDatabaseInitializationState() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configInitialized = false;
}

Status ShardingCatalogManager::_initConfigVersion(OperationContext* opCtx) {
    auto versionStatus =
        _localCatalogClient->getConfigVersion(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    if (versionStatus.isOK() || versionStatus != ErrorCodes::NoMatchingDocument) {
        return versionStatus.getStatus();
    }

    VersionType newVersion;
    newVersion.setClusterId(OID::gen());

    auto insertStatus = _localCatalogClient->insertConfigDocument(
        opCtx, NamespaceString::kConfigVersionNamespace, newVersion.toBSON(), kNoWaitWriteConcern);
    return insertStatus;
}

Status ShardingCatalogManager::_initConfigIndexes(OperationContext* opCtx) {
    const bool unique = true;

    Status result = createIndexesForConfigChunks(opCtx);
    if (result != Status::OK()) {
        return result;
    }

    result = createIndexOnConfigCollection(
        opCtx, NamespaceString::kConfigDatabasesNamespace, BSON("_id" << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create _id_ index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx, NamespaceString::kConfigsvrShardsNamespace, BSON(ShardType::host() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create host_1 index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx, TagsType::ConfigNS, BSON(TagsType::ns() << 1 << TagsType::min() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx, TagsType::ConfigNS, BSON(TagsType::ns() << 1 << TagsType::tag() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_tag_1 index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx,
        NamespaceString::kConfigQueryAnalyzersNamespace,
        BSON(analyze_shard_key::QueryAnalyzerDocument::kCollectionUuidFieldName << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create collUuid_1 index on config.queryAnalyzers");
    }

    auto status = createIndexOnConfigCollection(
        opCtx,
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        BSON(NamespacePlacementType::kNssFieldName
             << 1 << NamespacePlacementType::kTimestampFieldName << -1),
        true /*unique*/);

    if (!result.isOK()) {
        return result.withContext(
            "couldn't create nss_1_timestamp_-1 index on config.placementHistory");
    }

    return Status::OK();
}

/**
 * Ensure that config.collections exists upon configsvr startup
 */
Status ShardingCatalogManager::_initConfigCollections(OperationContext* opCtx) {
    // Ensure that config.collections exist so that snapshot reads on it don't fail with
    // SnapshotUnavailable error when it is implicitly created (when sharding a
    // collection for the first time) but not in yet in the committed snapshot).
    DBDirectClient client(opCtx);

    BSONObj cmd = BSON("create" << CollectionType::ConfigNS.coll());
    BSONObj result;
    const bool ok = client.runCommand(CollectionType::ConfigNS.dbName(), cmd, result);
    if (!ok) {  // create returns error NamespaceExists if collection already exists
        Status status = getStatusFromCommandResult(result);
        if (status != ErrorCodes::NamespaceExists) {
            return status.withContext("Could not create config.collections");
        }
    }
    return Status::OK();
}

Status ShardingCatalogManager::_initConfigSettings(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    const auto noopValidator =
        BSON("properties" << BSON(
                 "_id" << BSON("enum" << BSON_ARRAY(
                                   AutoMergeSettingsType::kKey
                                   << ReadWriteConcernDefaults::kPersistedDocumentId << "audit"))));
    const auto fullValidator =
        BSON("$jsonSchema" << BSON("oneOf" << BSON_ARRAY(BalancerSettingsType::kSchema
                                                         << ChunkSizeSettingsType::kSchema
                                                         << noopValidator)));

    BSONObj cmd = BSON("create" << NamespaceString::kConfigSettingsNamespace.coll());
    BSONObj result;
    const bool ok =
        client.runCommand(NamespaceString::kConfigSettingsNamespace.dbName(), cmd, result);
    if (!ok) {  // create returns error NamespaceExists if collection already exists
        Status status = getStatusFromCommandResult(result);
        if (status != ErrorCodes::NamespaceExists) {
            return status.withContext("Could not create config.settings");
        }
    }

    // Collection already exists, create validator on that collection
    CollMod collModCmd{NamespaceString::kConfigSettingsNamespace};
    collModCmd.getCollModRequest().setValidator(fullValidator);
    collModCmd.getCollModRequest().setValidationLevel(ValidationLevelEnum::strict);
    BSONObjBuilder builder;
    return processCollModCommand(
        opCtx, {NamespaceString::kConfigSettingsNamespace}, collModCmd, nullptr, &builder);
}

Status ShardingCatalogManager::setFeatureCompatibilityVersionOnShards(OperationContext* opCtx,
                                                                      const BSONObj& cmdObj) {

    // No shards should be added until we have forwarded featureCompatibilityVersion to all shards.
    Lock::SharedLock lk(opCtx, _kShardMembershipLock);

    // We do a direct read of the shards collection with local readConcern so no shards are missed,
    // but don't go through the ShardRegistry to prevent it from caching data that may be rolled
    // back.
    const auto opTimeWithShards =
        _localCatalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);

    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        if (shard->isConfig()) {
            // The config server will run shard upgrade/downgrade tasks directly instead of sending
            // a command to itself.
            continue;
        }

        auto response = shard->runCommand(opCtx,
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          DatabaseName::kAdmin,
                                          cmdObj,
                                          Shard::RetryPolicy::kIdempotent);
        if (!response.isOK()) {
            return response.getStatus();
        }
        if (!response.getValue().commandStatus.isOK()) {
            return response.getValue().commandStatus;
        }
        if (!response.getValue().writeConcernStatus.isOK()) {
            return response.getValue().writeConcernStatus;
        }
    }

    return Status::OK();
}

Status ShardingCatalogManager::runCloneAuthoritativeMetadataOnShards(OperationContext* opCtx) {
    // No shards should be added until we have forwarded the clone command to all shards.
    Lock::SharedLock lk(opCtx, _kShardMembershipLock);

    // We do a direct read of the shards collection with local readConcern so no shards are missed,
    // but don't go through the ShardRegistry to prevent it from caching data that may be rolled
    // back.
    const auto opTimeWithShards =
        _localCatalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);

    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        ShardsvrCloneAuthoritativeMetadata request;
        request.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
        request.setDbName(DatabaseName::kAdmin);

        auto response = shard->runCommand(opCtx,
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          DatabaseName::kAdmin,
                                          request.toBSON(),
                                          Shard::RetryPolicy::kIdempotent);

        if (!response.isOK()) {
            return response.getStatus();
        }
        if (!response.getValue().commandStatus.isOK()) {
            return response.getValue().commandStatus;
        }
        if (!response.getValue().writeConcernStatus.isOK()) {
            return response.getValue().writeConcernStatus;
        }
    }

    return Status::OK();
}

StatusWith<bool> ShardingCatalogManager::_isShardRequiredByZoneStillInUse(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const std::string& shardName,
    const std::string& zoneName) {
    auto findShardStatus =
        _localConfigShard->exhaustiveFindOnConfig(opCtx,
                                                  readPref,
                                                  repl::ReadConcernLevel::kLocalReadConcern,
                                                  NamespaceString::kConfigsvrShardsNamespace,
                                                  BSON(ShardType::tags() << zoneName),
                                                  BSONObj(),
                                                  2);

    if (!findShardStatus.isOK()) {
        return findShardStatus.getStatus();
    }

    const auto shardDocs = findShardStatus.getValue().docs;

    if (shardDocs.size() == 0) {
        // The zone doesn't exists.
        return false;
    }

    if (shardDocs.size() == 1) {
        auto shardDocStatus = ShardType::fromBSON(shardDocs.front());
        if (!shardDocStatus.isOK()) {
            return shardDocStatus.getStatus();
        }

        auto shardDoc = shardDocStatus.getValue();
        if (shardDoc.getName() != shardName) {
            // The last shard that belongs to this zone is a different shard.
            return false;
        }

        auto findChunkRangeStatus =
            _localConfigShard->exhaustiveFindOnConfig(opCtx,
                                                      readPref,
                                                      repl::ReadConcernLevel::kLocalReadConcern,
                                                      TagsType::ConfigNS,
                                                      BSON(TagsType::tag() << zoneName),
                                                      BSONObj(),
                                                      1);

        if (!findChunkRangeStatus.isOK()) {
            return findChunkRangeStatus.getStatus();
        }

        return findChunkRangeStatus.getValue().docs.size() > 0;
    }

    return false;
}

BSONObj ShardingCatalogManager::writeToConfigDocumentInTxn(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const BatchedCommandRequest& request,
                                                           TxnNumber txnNumber) {
    invariant(nss.dbName() == DatabaseName::kConfig);
    auto response =
        runCommandInLocalTxn(
            opCtx, nss.dbName(), false /* startTransaction */, txnNumber, request.toBSON())
            .body;

    uassertStatusOK(getStatusFromWriteCommandReply(response));

    return response;
}

void ShardingCatalogManager::insertConfigDocuments(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   std::vector<BSONObj> docs,
                                                   boost::optional<TxnNumber> txnNumber) {
    invariant(nss.dbName() == DatabaseName::kConfig);

    // if the operation is in a transaction then the overhead for each document is different.
    const auto documentOverhead = txnNumber
        ? write_ops::kWriteCommandBSONArrayPerElementOverheadBytes
        : write_ops::kRetryableAndTxnBatchWriteBSONSizeOverhead;

    std::vector<std::vector<BSONObj>> batches = createBulkWriteBatches(docs, documentOverhead);

    std::for_each(batches.begin(), batches.end(), [&](const std::vector<BSONObj>& batch) {
        BatchedCommandRequest request([nss, batch] {
            write_ops::InsertCommandRequest insertOp(nss);
            insertOp.setDocuments(batch);
            return insertOp;
        }());

        if (txnNumber) {
            writeToConfigDocumentInTxn(opCtx, nss, request, txnNumber.value());
        } else {
            uassertStatusOK(
                getStatusFromWriteCommandReply(executeConfigRequest(opCtx, nss, request)));
        }
    });
}

boost::optional<BSONObj> ShardingCatalogManager::findOneConfigDocumentInTxn(
    OperationContext* opCtx,
    const NamespaceString& nss,
    TxnNumber txnNumber,
    const BSONObj& query) {

    invariant(nss.dbName() == DatabaseName::kConfig);

    FindCommandRequest findCommand(nss);
    findCommand.setFilter(query);
    findCommand.setSingleBatch(true);
    findCommand.setLimit(1);

    auto res = runCommandInLocalTxn(
                   opCtx, nss.dbName(), false /*startTransaction*/, txnNumber, findCommand.toBSON())
                   .body;
    uassertStatusOK(getStatusFromCommandResult(res));

    auto cursor = uassertStatusOK(CursorResponse::parseFromBSON(res));
    auto result = cursor.releaseBatch();

    if (result.empty()) {
        return boost::none;
    }

    return result.front().getOwned();
}

BSONObj ShardingCatalogManager::findOneConfigDocument(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const BSONObj& query) {
    invariant(nss.isConfigDB());

    FindCommandRequest findCommand(nss);
    findCommand.setFilter(query);

    DBDirectClient client(opCtx);
    return client.findOne(findCommand);
}

Status ShardingCatalogManager::checkTimeseriesShardKeys(OperationContext* opCtx,
                                                        const DatabaseName& dbName) {

    // The following aggregation pipeline collects all the collections that are timeseries and are
    // sharded based on the timeField key. This supposed to run on the config.collections
    // collection.
    //
    // The stages are the following:
    //
    // 1. Match all the documents that has timeseriesField property. This is only true for the
    // timeseries collections
    //
    // 2. Add shardKeys and viewName fields.
    // The shardKeys will be an array with the keys of the elements in the original `key` field.
    // Eg: { meta.sensorID: 1, timestamp: 1 } -> [ "meta.sensorID", "timestamp" ]
    // The viewName will be the value of the `_id` field without the `.system.buckets` part.
    // Eg: `test.system.buckets.shardedCollectionName` -> `test.shardedCollectionName`
    //
    // 3. Project the timeShardKey, timeField and viewName into a new document
    // The timeShardKey is the previously added shardKeys where the element matches the
    // `.*\.$timeseriesFields.timeField` pattern where $timeseriesFields.timeField is the timeField
    // of the original timeseries collection.
    // Eg: [ "meta.sensorID", "timestamp" ] -> [ "timestamp" ]
    // The timeField is the field name of the timeField in the original timeseries collection
    // The viewName is the same as previously explained
    //
    // 4. Match all documents where the timeShardKey is not empty
    static const auto rawPipelineStages = [] {
        auto rawPipelineBSON = fromjson(R"({pipeline: [
            {
                "$match":{
                    "timeseriesFields":{
                        "$ne":null
                    }
                }
            },
            {
                "$addFields":{
                    "shardKeys":{
                        "$map":{
                            "input":{
                                "$objectToArray":"$key"
                            },
                            "in":"$$this.k"
                        }
                    },
                    "viewName":{
                        "$replaceOne":{
                            "input":"$_id",
                            "find":".system.buckets",
                            "replacement":""
                        }
                    }
                }
            },
            {
                "$project":{
                    "timeShardKey":{
                        "$filter":{
                            "input":"$shardKeys",
                            "cond":{
                                "$regexMatch":{
                                    "input":"$$this",
                                    "regex":{
                                        "$concat":[
                                            ".*\\.",
                                            "$timeseriesFields.timeField"
                                        ]
                                    }
                                }
                            }
                        }
                    },
                    "timeField":"$timeseriesFields.timeField",
                    "viewName":"$viewName"
                }
            },
            {
                "$match":{
                    "timeShardKey":{
                        "$ne":[]
                    }
                }
            }
        ]})");
        return parsePipelineFromBSON(rawPipelineBSON.firstElement());
    }();

    AggregateCommandRequest timeFieldIndexedTimeSeriesAggRequest{
        NamespaceString::kConfigsvrCollectionsNamespace, rawPipelineStages};
    auto documents =
        _localCatalogClient->runCatalogAggregation(opCtx,
                                                   timeFieldIndexedTimeSeriesAggRequest,
                                                   {repl::ReadConcernLevel::kMajorityReadConcern});

    for (auto&& document : documents) {
        const auto viewNameField = document.getField("viewName");
        const auto viewName = NamespaceStringUtil::deserialize(
            boost::none, viewNameField.String(), SerializationContext::stateDefault());

        const auto timeFieldField = document.getField("timeField");
        const auto timeField = timeFieldField.String();

        LOGV2_WARNING(
            8864701,
            "The time-series collection is currently using timeField as a shard key. Sharding on "
            "time will be disabled in future versions. Please reshard your collection using "
            "metaField as recommended in our time-series sharding documentation.",
            logAttrs(viewName),
            "timeField"_attr = timeField);
    }

    return Status::OK();
}

void ShardingCatalogManager::withTransactionAPI(OperationContext* opCtx,
                                                const NamespaceString& namespaceForInitialFind,
                                                txn_api::Callback callback) {
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);
    txn.run(opCtx,
            [innerCallback = std::move(callback),
             namespaceForInitialFind](const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) -> SemiFuture<void> {
                // Begin the transaction with a noop find.
                FindCommandRequest findCommand(namespaceForInitialFind);
                findCommand.setBatchSize(0);
                findCommand.setSingleBatch(true);
                return txnClient.exhaustiveFind(findCommand)
                    .thenRunOn(txnExec)
                    .then([&innerCallback, &txnClient, txnExec](auto foundDocs) {
                        return innerCallback(txnClient, txnExec);
                    })
                    .semi();
            });
}

void ShardingCatalogManager::withTransaction(
    OperationContext* opCtx,
    const NamespaceString& namespaceForInitialFind,
    unique_function<void(OperationContext*, TxnNumber)> func) {
    withTransaction(
        opCtx, namespaceForInitialFind, std::move(func), defaultMajorityWriteConcernDoNotUse());
}

void ShardingCatalogManager::withTransaction(
    OperationContext* opCtx,
    const NamespaceString& namespaceForInitialFind,
    unique_function<void(OperationContext*, TxnNumber)> func,
    const WriteConcernOptions& writeConcern) {

    AlternativeSessionRegion asr(opCtx);
    auto* const client = asr.opCtx()->getClient();
    asr.opCtx()->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    AuthorizationSession::get(client)->grantInternalAuthorization();
    TxnNumber txnNumber = 0;

    ScopeGuard guard([opCtx = asr.opCtx(), &txnNumber, &writeConcern] {
        try {
            abortTransaction(opCtx, txnNumber, writeConcern);
        } catch (DBException& e) {
            LOGV2_WARNING(5192100,
                          "Failed to abort transaction in AlternativeSessionRegion",
                          "error"_attr = redact(e));
        }
    });

    size_t attempt = 1;
    while (true) {
        // We retry on transient transaction errors like LockTimeout and detect whether
        // asr.opCtx() was killed by explicitly checking if it has been interrupted.
        asr.opCtx()->checkForInterrupt();
        ++txnNumber;

        // We stop retrying on ErrorCategory::NotPrimaryError and ErrorCategory::ShutdownError
        // exceptions because it is expected for another attempt on this same server to keep
        // receiving that error.
        try {
            startTransactionWithNoopFind(asr.opCtx(), namespaceForInitialFind, txnNumber);
            func(asr.opCtx(), txnNumber);
        } catch (const ExceptionFor<ErrorCategory::NotPrimaryError>&) {
            throw;
        } catch (const ExceptionFor<ErrorCategory::ShutdownError>&) {
            throw;
        } catch (const DBException& ex) {
            if (isTransientTransactionError(
                    ex.code(), false /* hasWriteConcernError */, false /* isCommitOrAbort */)) {
                logAndBackoff(5108800,
                              ::mongo::logv2::LogComponent::kSharding,
                              logv2::LogSeverity::Debug(1),
                              attempt++,
                              "Transient transaction error while running local replica set"
                              " transaction, retrying",
                              "reason"_attr = redact(ex.toStatus()));
                continue;
            }
            throw;
        }

        auto [cmdStatus, wcStatus] = commitTransaction(asr.opCtx(), txnNumber, writeConcern);
        if (!cmdStatus.isOK() && !cmdStatus.isA<ErrorCategory::NotPrimaryError>() &&
            !cmdStatus.isA<ErrorCategory::ShutdownError>() &&
            isTransientTransactionError(
                cmdStatus.code(), !wcStatus.isOK(), true /* isCommitOrAbort */)) {
            logAndBackoff(5108801,
                          ::mongo::logv2::LogComponent::kSharding,
                          logv2::LogSeverity::Debug(1),
                          attempt++,
                          "Transient transaction error while committing local replica set"
                          " transaction, retrying",
                          "reason"_attr = redact(cmdStatus));
            continue;
        }

        uassertStatusOK(cmdStatus);
        // commitTransaction() specifies {writeConcern: {w: "majority"}} without a wtimeout, so
        // it isn't expected to have a write concern error unless the primary is stepping down
        // or shutting down or asr.opCtx() is killed. We throw because all of those cases are
        // terminal for the caller running a local replica set transaction anyway.
        uassertStatusOK(wcStatus);
        shardingCatalogManagerWithTransactionFailWCAfterCommit.execute([&](const BSONObj& data) {
            // Simulates the case described in the above comment where the transaction commits, but
            // fails to replicate due to some interruption.
            if (!writeConcern.needToWaitForOtherNodes()) {
                return;
            }
            uasserted(ErrorCodes::Interrupted,
                      "Failpoint shardingCatalogManagerWithTransactionFailWCAfterCommit");
        });

        guard.dismiss();
        return;
    }
}

}  // namespace mongo
