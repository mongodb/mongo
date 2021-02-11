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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <tuple>

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/type_lockpings.h"
#include "mongo/db/s/type_locks.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log_and_backoff.h"

namespace mongo {
namespace {

const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

// This value is initialized only if the node is running as a config server
const auto getShardingCatalogManager =
    ServiceContext::declareDecoration<boost::optional<ShardingCatalogManager>>();

OpMsg runCommandInLocalTxn(OperationContext* opCtx,
                           StringData db,
                           bool startTransaction,
                           TxnNumber txnNumber,
                           BSONObj cmdObj) {
    BSONObjBuilder bob(std::move(cmdObj));
    if (startTransaction) {
        bob.append("startTransaction", true);
    }
    bob.append("autocommit", false);
    bob.append(OperationSessionInfo::kTxnNumberFieldName, txnNumber);

    BSONObjBuilder lsidBuilder(bob.subobjStart("lsid"));
    opCtx->getLogicalSessionId()->serialize(&bob);
    lsidBuilder.doneFast();

    return OpMsg::parseOwned(
        opCtx->getServiceContext()
            ->getServiceEntryPoint()
            ->handleRequest(opCtx,
                            OpMsgRequest::fromDBAndBody(db.toString(), bob.obj()).serialize())
            .get()
            .response);
}

void startTransactionWithNoopFind(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  TxnNumber txnNumber) {
    FindCommand findCommand(nss);
    findCommand.setBatchSize(0);
    findCommand.setSingleBatch(true);

    auto res =
        runCommandInLocalTxn(
            opCtx, nss.db(), true /*startTransaction*/, txnNumber, findCommand.toBSON(BSONObj()))
            .body;
    uassertStatusOK(getStatusFromCommandResult(res));
}

BSONObj commitOrAbortTransaction(OperationContext* opCtx,
                                 TxnNumber txnNumber,
                                 std::string cmdName) {
    // Swap out the clients in order to get a fresh opCtx. Previous operations in this transaction
    // that have been run on this opCtx would have set the timeout in the locker on the opCtx, but
    // commit should not have a lock timeout.
    auto newClient = getGlobalServiceContext()->makeClient("ShardingCatalogManager");
    {
        stdx::lock_guard<Client> lk(*newClient);
        newClient->setSystemOperationKillableByStepdown(lk);
    }
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    newOpCtx->setAlwaysInterruptAtStepDownOrUp();
    AuthorizationSession::get(newOpCtx.get()->getClient())
        ->grantInternalAuthorization(newOpCtx.get()->getClient());
    newOpCtx.get()->setLogicalSessionId(opCtx->getLogicalSessionId().get());
    newOpCtx.get()->setTxnNumber(txnNumber);

    BSONObjBuilder bob;
    bob.append(cmdName, true);
    bob.append("autocommit", false);
    bob.append(OperationSessionInfo::kTxnNumberFieldName, txnNumber);
    bob.append(WriteConcernOptions::kWriteConcernField, WriteConcernOptions::Majority);

    BSONObjBuilder lsidBuilder(bob.subobjStart("lsid"));
    newOpCtx->getLogicalSessionId()->serialize(&bob);
    lsidBuilder.doneFast();

    const auto cmdObj = bob.obj();

    const auto replyOpMsg =
        OpMsg::parseOwned(newOpCtx->getServiceContext()
                              ->getServiceEntryPoint()
                              ->handleRequest(newOpCtx.get(),
                                              OpMsgRequest::fromDBAndBody(
                                                  NamespaceString::kAdminDb.toString(), cmdObj)
                                                  .serialize())
                              .get()
                              .response);
    return replyOpMsg.body;
}

// Runs commit for the transaction with 'txnNumber'.
auto commitTransaction(OperationContext* opCtx, TxnNumber txnNumber) {
    auto response = commitOrAbortTransaction(opCtx, txnNumber, "commitTransaction");
    return std::make_tuple(getStatusFromCommandResult(response),
                           getWriteConcernStatusFromCommandResult(response));
}

// Runs abort for the transaction with 'txnNumber'.
void abortTransaction(OperationContext* opCtx, TxnNumber txnNumber) {
    auto response = commitOrAbortTransaction(opCtx, txnNumber, "abortTransaction");

    // It is safe to ignore write concern errors in the presence of a NoSuchTransaction command
    // error because the transaction being aborted was both generated by and run locally on this
    // replica set primary. The NoSuchTransaction decision couldn't end up being rolled back.
    auto status = getStatusFromCommandResult(response);
    if (status.code() != ErrorCodes::NoSuchTransaction) {
        uassertStatusOK(status);
        uassertStatusOK(getWriteConcernStatusFromCommandResult(response));
    }
}

// Updates documents in the config db using DBDirectClient
void updateConfigDocumentDBDirect(OperationContext* opCtx,
                                  const mongo::NamespaceString& nss,
                                  const BSONObj& query,
                                  const BSONObj& update,
                                  bool upsert,
                                  bool multi) {
    invariant(nss.db() == "config");

    DBDirectClient client(opCtx);

    write_ops::Update updateOp(nss, [&] {
        write_ops::UpdateOpEntry u;
        u.setQ(query);
        u.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
        u.setMulti(multi);
        u.setUpsert(upsert);
        return std::vector{u};
    }());
    updateOp.setWriteCommandBase([] {
        write_ops::WriteCommandBase base;
        base.setOrdered(false);
        return base;
    }());

    auto commandResult = client.runCommand(OpMsgRequest::fromDBAndBody(
        nss.db(), updateOp.toBSON(ShardingCatalogClient::kMajorityWriteConcern.toBSON())));
    uassertStatusOK([&] {
        BatchedCommandResponse response;
        std::string unusedErrmsg;
        response.parseBSON(
            commandResult->getCommandReply(),
            &unusedErrmsg);  // Return value intentionally ignored, because response.toStatus() will
                             // contain any errors in more detail
        return response.toStatus();
    }());
    uassertStatusOK(getWriteConcernStatusFromCommandResult(commandResult->getCommandReply()));
}

Status createNsIndexesForConfigChunks(OperationContext* opCtx) {
    const bool unique = true;
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    Status result = configShard->createIndexOnConfig(
        opCtx, ChunkType::ConfigNS, BSON(ChunkType::ns() << 1 << ChunkType::min() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::ns() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_shard_1_min_1 index on config db");
    }

    result =
        configShard->createIndexOnConfig(opCtx,
                                         ChunkType::ConfigNS,
                                         BSON(ChunkType::ns() << 1 << ChunkType::lastmod() << 1),
                                         unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_lastmod_1 index on config db");
    }

    return Status::OK();
}

Status createUuidIndexesForConfigChunks(OperationContext* opCtx) {
    const bool unique = true;
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    Status result = configShard->createIndexOnConfig(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_min_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_shard_1_min_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::lastmod() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_lastmod_1 index on config db");
    }

    return Status::OK();
}

}  // namespace

void ShardingCatalogManager::create(ServiceContext* serviceContext,
                                    std::unique_ptr<executor::TaskExecutor> addShardExecutor) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(!shardingCatalogManager);

    shardingCatalogManager.emplace(serviceContext, std::move(addShardExecutor));
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
    ServiceContext* serviceContext, std::unique_ptr<executor::TaskExecutor> addShardExecutor)
    : _serviceContext(serviceContext),
      _executorForAddShard(std::move(addShardExecutor)),
      _kShardMembershipLock("shardMembershipLock"),
      _kChunkOpLock("chunkOpLock"),
      _kZoneOpLock("zoneOpLock") {
    startup();
}

ShardingCatalogManager::~ShardingCatalogManager() {
    shutDown();
}

void ShardingCatalogManager::startup() {
    stdx::lock_guard<Latch> lk(_mutex);
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
        stdx::lock_guard<Latch> lk(_mutex);
        if (_configInitialized) {
            return {ErrorCodes::AlreadyInitialized,
                    "Config database was previously loaded into memory"};
        }
    }

    Status status = _initConfigIndexes(opCtx);
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

    stdx::lock_guard<Latch> lk(_mutex);
    _configInitialized = true;

    return Status::OK();
}

void ShardingCatalogManager::discardCachedConfigDatabaseInitializationState() {
    stdx::lock_guard<Latch> lk(_mutex);
    _configInitialized = false;
}

Status ShardingCatalogManager::_initConfigVersion(OperationContext* opCtx) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    auto versionStatus =
        catalogClient->getConfigVersion(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    if (!versionStatus.isOK()) {
        return versionStatus.getStatus();
    }

    const auto& versionInfo = versionStatus.getValue();
    if (versionInfo.getMinCompatibleVersion() > CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "current version v" << CURRENT_CONFIG_VERSION
                              << " is older than the cluster min compatible v"
                              << versionInfo.getMinCompatibleVersion()};
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_EmptyVersion) {
        VersionType newVersion;
        newVersion.setClusterId(OID::gen());
        newVersion.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
        newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);

        BSONObj versionObj(newVersion.toBSON());
        auto insertStatus = catalogClient->insertConfigDocument(
            opCtx, VersionType::ConfigNS, versionObj, kNoWaitWriteConcern);

        return insertStatus;
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_UnreportedVersion) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                "Assuming config data is old since the version document cannot be found in the "
                "config server and it contains databases besides 'local' and 'admin'. "
                "Please upgrade if this is the case. Otherwise, make sure that the config "
                "server is clean."};
    }

    if (versionInfo.getCurrentVersion() < CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "need to upgrade current cluster version to v"
                              << CURRENT_CONFIG_VERSION << "; currently at v"
                              << versionInfo.getCurrentVersion()};
    }

    return Status::OK();
}

Status ShardingCatalogManager::_initConfigIndexes(OperationContext* opCtx) {
    const bool unique = true;
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        const auto result = createUuidIndexesForConfigChunks(opCtx);
        if (result != Status::OK()) {
            return result;
        }

    } else {
        const auto result = createNsIndexesForConfigChunks(opCtx);
        if (result != Status::OK()) {
            return result;
        }
    }

    Status result = configShard->createIndexOnConfig(
        opCtx,
        MigrationType::ConfigNS,
        BSON(MigrationType::ns() << 1 << MigrationType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config.migrations");
    }

    result = configShard->createIndexOnConfig(
        opCtx, ShardType::ConfigNS, BSON(ShardType::host() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create host_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, LocksType::ConfigNS, BSON(LocksType::lockID() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create lock id index on config db");
    }

    result =
        configShard->createIndexOnConfig(opCtx,
                                         LocksType::ConfigNS,
                                         BSON(LocksType::state() << 1 << LocksType::process() << 1),
                                         !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create state and process id index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, LockpingsType::ConfigNS, BSON(LockpingsType::ping() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create lockping ping time index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, TagsType::ConfigNS, BSON(TagsType::ns() << 1 << TagsType::min() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, TagsType::ConfigNS, BSON(TagsType::ns() << 1 << TagsType::tag() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_tag_1 index on config db");
    }

    return Status::OK();
}

Status ShardingCatalogManager::setFeatureCompatibilityVersionOnShards(OperationContext* opCtx,
                                                                      const BSONObj& cmdObj) {

    // No shards should be added until we have forwarded featureCompatibilityVersion to all shards.
    Lock::SharedLock lk(opCtx->lockState(), _kShardMembershipLock);

    // We do a direct read of the shards collection with local readConcern so no shards are missed,
    // but don't go through the ShardRegistry to prevent it from caching data that may be rolled
    // back.
    const auto opTimeWithShards = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getAllShards(
        opCtx, repl::ReadConcernLevel::kLocalReadConcern));

    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        auto response = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
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

void ShardingCatalogManager::removePre49LegacyMetadata(OperationContext* opCtx) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    DBDirectClient client(opCtx);

    // Delete all documents which have {dropped: true} from config.collections
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             CollectionType::ConfigNS,
                                             BSON("dropped" << true),
                                             ShardingCatalogClient::kLocalWriteConcern));

    // Clear the {dropped:true} and {distributionMode:sharded} fields from config.collections
    write_ops::Update clearDroppedAndDistributionMode(CollectionType::ConfigNS, [] {
        write_ops::UpdateOpEntry u;
        u.setQ({});
        u.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON("$unset"
                                                                          << BSON("dropped"
                                                                                  << "")
                                                                          << "$unset"
                                                                          << BSON("distributionMode"
                                                                                  << ""))));
        u.setMulti(true);
        return std::vector{u};
    }());
    clearDroppedAndDistributionMode.setWriteCommandBase([] {
        write_ops::WriteCommandBase base;
        base.setOrdered(false);
        return base;
    }());

    auto commandResult = client.runCommand(
        OpMsgRequest::fromDBAndBody(CollectionType::ConfigNS.db(),
                                    clearDroppedAndDistributionMode.toBSON(
                                        ShardingCatalogClient::kMajorityWriteConcern.toBSON())));
    uassertStatusOK([&] {
        BatchedCommandResponse response;
        std::string unusedErrmsg;
        response.parseBSON(
            commandResult->getCommandReply(),
            &unusedErrmsg);  // Return value intentionally ignored, because response.toStatus() will
                             // contain any errors in more detail
        return response.toStatus();
    }());
    uassertStatusOK(getWriteConcernStatusFromCommandResult(commandResult->getCommandReply()));
}

void ShardingCatalogManager::upgradeMetadataFor49(OperationContext* opCtx) {
    LOGV2(5276704, "Starting metadata upgrade to 4.9");

    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabledAndIgnoreFCV()) {
        _createDBTimestampsFor49(opCtx);
        _upgradeCollectionsAndChunksMetadataFor49(opCtx);
    }

    LOGV2(5276705, "Successfully upgraded metadata to 4.9");
}

void ShardingCatalogManager::downgradeMetadataToPre49(OperationContext* opCtx) {
    LOGV2(5276706, "Starting metadata downgrade to pre 4.9");

    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabledAndIgnoreFCV()) {
        _downgradeConfigDatabasesEntriesToPre49(opCtx);
        _downgradeCollectionsAndChunksMetadataToPre49(opCtx);
    }

    LOGV2(5276707, "Successfully downgraded metadata to pre 4.9");
}

void ShardingCatalogManager::_createDBTimestampsFor49(OperationContext* opCtx) {
    LOGV2(5258802, "Starting upgrade of config.databases");

    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto const catalogCache = Grid::get(opCtx)->catalogCache();
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto dbDocs =
        uassertStatusOK(
            configShard->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                DatabaseType::ConfigNS,
                BSON(DatabaseType::version() + "." + DatabaseVersion::kTimestampFieldName
                     << BSON("$exists" << false)),
                BSONObj(),
                boost::none))
            .docs;


    for (const auto& doc : dbDocs) {
        const DatabaseType db = uassertStatusOK(DatabaseType::fromBSON(doc));
        const auto name = db.getName();

        auto now = VectorClock::get(opCtx)->getTime();
        auto clusterTime = now.clusterTime().asTimestamp();

        uassertStatusOK(catalogClient->updateConfigDocument(
            opCtx,
            DatabaseType::ConfigNS,
            BSON(DatabaseType::name << name),
            BSON("$set" << BSON(DatabaseType::version() + "." + DatabaseVersion::kTimestampFieldName
                                << clusterTime)),
            false /* upsert */,
            ShardingCatalogClient::kMajorityWriteConcern));

        catalogCache->invalidateDatabaseEntry_LINEARIZABLE(name);
    }

    LOGV2(5258803, "Successfully upgraded config.databases");
}

void ShardingCatalogManager::_downgradeConfigDatabasesEntriesToPre49(OperationContext* opCtx) {
    LOGV2(5258806, "Starting downgrade of config.databases");

    DBDirectClient client(opCtx);

    // Clear the 'timestamp' fields from config.databases
    write_ops::Update unsetTimestamp(DatabaseType::ConfigNS, [] {
        write_ops::UpdateOpEntry u;
        u.setQ({});
        u.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(
                     DatabaseType::version() + "." + DatabaseVersion::kTimestampFieldName << ""))));
        u.setMulti(true);
        return std::vector{u};
    }());
    unsetTimestamp.setWriteCommandBase([] {
        write_ops::WriteCommandBase base;
        base.setOrdered(false);
        return base;
    }());

    auto commandResult = client.runCommand(OpMsgRequest::fromDBAndBody(
        DatabaseType::ConfigNS.db(),
        unsetTimestamp.toBSON(ShardingCatalogClient::kMajorityWriteConcern.toBSON())));

    uassertStatusOK([&] {
        BatchedCommandResponse response;
        std::string unusedErrmsg;
        response.parseBSON(
            commandResult->getCommandReply(),
            &unusedErrmsg);  // Return value intentionally ignored, because response.toStatus()
                             // will contain any errors in more detail
        return response.toStatus();
    }());
    uassertStatusOK(getWriteConcernStatusFromCommandResult(commandResult->getCommandReply()));

    LOGV2(5258807, "Successfully downgraded config.databases");
}

void ShardingCatalogManager::_upgradeCollectionsAndChunksMetadataFor49(OperationContext* opCtx) {
    LOGV2(5276700, "Starting upgrade of config.collections and config.chunks");

    auto const catalogCache = Grid::get(opCtx)->catalogCache();
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    DistLockManager::ScopedDistLock dbDistLock(uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx,
        DistLockManager::kShardingRoutingInfoFormatStabilityLockName,
        "fcvUpgrade",
        DistLockManager::kDefaultLockTimeout)));

    auto collectionDocs =
        uassertStatusOK(
            configShard->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                CollectionType::ConfigNS,
                BSONObj(BSON(CollectionType::kTimestampFieldName << BSON("$exists" << false))),
                BSONObj(),
                boost::none))
            .docs;

    stdx::unordered_map<NamespaceString, Timestamp> timestampMap;

    // Set timestamp and uuid for all chunks in all collections
    for (const auto& doc : collectionDocs) {
        const CollectionType coll(doc);
        const auto uuid = coll.getUuid();
        const auto nss = coll.getNss();

        const auto now = VectorClock::get(opCtx)->getTime();
        const auto newTimestamp = now.clusterTime().asTimestamp();
        timestampMap.emplace(nss, newTimestamp);

        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations.
        Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

        updateConfigDocumentDBDirect(
            opCtx,
            ChunkType::ConfigNS,
            BSON(ChunkType::ns(nss.ns())) /* query */,
            BSON("$set" << BSON(ChunkType::timestamp(newTimestamp)
                                << ChunkType::collectionUUID() << uuid)) /* update */,
            false /* upsert */,
            true /* multi */);
    }

    // Create uuid_* indexes for config.chunks
    uassertStatusOK(createUuidIndexesForConfigChunks(opCtx));

    // Set timestamp for all collections in config.collections
    for (const auto& doc : collectionDocs) {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations.
        Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

        const CollectionType coll(doc);
        const auto nss = coll.getNss();

        updateConfigDocumentDBDirect(opCtx,
                                     CollectionType::ConfigNS,
                                     BSON(CollectionType::kNssFieldName << nss.ns()) /* query */,
                                     BSON("$set" << BSON(CollectionType::kTimestampFieldName
                                                         << timestampMap.at(nss))) /* update */,
                                     false /* upsert */,
                                     false /* multi */);

        catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
    }

    // Drop ns_* indexes of config.chunks
    {
        DBDirectClient client(opCtx);

        const bool includeBuildUUIDs = false;
        const int options = 0;
        auto indexes = client.getIndexSpecs(ChunkType::ConfigNS, includeBuildUUIDs, options);
        BSONArrayBuilder indexNamesToDrop;
        for (const auto& index : indexes) {
            const auto indexName = index.getStringField("name");
            if (indexName == "ns_1_min_1"_sd || indexName == "ns_1_shard_1_min_1"_sd ||
                indexName == "ns_1_lastmod_1"_sd) {
                indexNamesToDrop.append(indexName);
            }
        }

        BSONObj info;
        if (!client.runCommand(ChunkType::ConfigNS.db().toString(),
                               BSON("dropIndexes" << ChunkType::ConfigNS.coll() << "index"
                                                  << indexNamesToDrop.arr()),
                               info))
            uassertStatusOK(getStatusFromCommandResult(info));
    }

    // Unset ns for all chunks on config.chunks
    {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations.
        Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

        updateConfigDocumentDBDirect(opCtx,
                                     ChunkType::ConfigNS,
                                     {} /* query */,
                                     BSON("$unset" << BSON(ChunkType::ns(""))) /* update */,
                                     false /* upsert */,
                                     true /* multi */);
    }

    LOGV2(5276701, "Successfully upgraded config.collections and config.chunks");
}

void ShardingCatalogManager::_downgradeCollectionsAndChunksMetadataToPre49(
    OperationContext* opCtx) {
    LOGV2(5276702, "Starting downgrade of config.collections and config.chunks");

    auto const catalogCache = Grid::get(opCtx)->catalogCache();
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    DistLockManager::ScopedDistLock dbDistLock(uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx,
        DistLockManager::kShardingRoutingInfoFormatStabilityLockName,
        "fcvDowngrade",
        DistLockManager::kDefaultLockTimeout)));

    auto collectionDocs =
        uassertStatusOK(
            configShard->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                CollectionType::ConfigNS,
                BSONObj(BSON(CollectionType::kTimestampFieldName << BSON("$exists" << true))),
                BSONObj(),
                boost::none))
            .docs;

    // Set ns on all chunks
    for (const auto& doc : collectionDocs) {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations.
        Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

        const CollectionType coll(doc);
        const auto uuid = coll.getUuid();
        const auto nss = coll.getNss();

        updateConfigDocumentDBDirect(opCtx,
                                     ChunkType::ConfigNS,
                                     BSON(ChunkType::collectionUUID << uuid) /* query */,
                                     BSON("$set" << BSON(ChunkType::ns(nss.ns()))) /* update */,
                                     false /* upsert */,
                                     true /* multi */);
    }

    // Create ns_* indexes for config.chunks
    uassertStatusOK(createNsIndexesForConfigChunks(opCtx));

    // Unset timestamp for all collections
    for (const auto& doc : collectionDocs) {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations.
        Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

        const CollectionType coll(doc);
        const auto nss = coll.getNss();

        updateConfigDocumentDBDirect(
            opCtx,
            CollectionType::ConfigNS,
            BSON(CollectionType::kNssFieldName << nss.ns()) /* query */,
            BSON("$unset" << BSON(CollectionType::kTimestampFieldName << "")) /* update */,
            false /* upsert */,
            false /* multi */);

        catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
    }

    // Drop uuid_* indexes for config.chunks
    {
        DBDirectClient client(opCtx);

        const bool includeBuildUUIDs = false;
        const int options = 0;
        auto indexes = client.getIndexSpecs(ChunkType::ConfigNS, includeBuildUUIDs, options);
        BSONArrayBuilder indexNamesToDrop;
        for (const auto& index : indexes) {
            const auto indexName = index.getStringField("name");
            if (indexName == "uuid_1_min_1"_sd || indexName == "uuid_1_shard_1_min_1"_sd ||
                indexName == "uuid_1_lastmod_1"_sd) {
                indexNamesToDrop.append(indexName);
            }
        }

        BSONObj info;
        if (!client.runCommand(ChunkType::ConfigNS.db().toString(),
                               BSON("dropIndexes" << ChunkType::ConfigNS.coll() << "index"
                                                  << indexNamesToDrop.arr()),
                               info))
            uassertStatusOK(getStatusFromCommandResult(info));
    }

    // Unset uuid for all chunks on config.chunks
    {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations.
        Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

        updateConfigDocumentDBDirect(
            opCtx,
            ChunkType::ConfigNS,
            {} /* query */,
            BSON("$unset" << BSON(ChunkType::timestamp.name()
                                  << "" << ChunkType::collectionUUID() << "")) /* update */,
            false /* upsert */,
            true /* multi */);
    }


    LOGV2(5276703, "Successfully downgraded config.collections and config.chunks");
}

Lock::ExclusiveLock ShardingCatalogManager::lockZoneMutex(OperationContext* opCtx) {
    Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);
    return lk;
}

StatusWith<bool> ShardingCatalogManager::_isShardRequiredByZoneStillInUse(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const std::string& shardName,
    const std::string& zoneName) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findShardStatus =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            readPref,
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ShardType::ConfigNS,
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
            configShard->exhaustiveFindOnConfig(opCtx,
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
    invariant(nss.db() == NamespaceString::kConfigDb);
    auto response = runCommandInLocalTxn(
                        opCtx, nss.db(), false /* startTransaction */, txnNumber, request.toBSON())
                        .body;

    uassertStatusOK(getStatusFromCommandResult(response));
    uassertStatusOK(getWriteConcernStatusFromCommandResult(response));

    return response;
}

void ShardingCatalogManager::insertConfigDocumentsInTxn(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        std::vector<BSONObj> docs,
                                                        TxnNumber txnNumber) {
    invariant(nss.db() == NamespaceString::kConfigDb);

    std::vector<BSONObj> workingBatch;
    size_t workingBatchItemSize = 0;
    int workingBatchDocSize = 0;

    auto doBatchInsert = [&]() {
        BatchedCommandRequest request([&] {
            write_ops::Insert insertOp(nss);
            insertOp.setDocuments(workingBatch);
            return insertOp;
        }());

        writeToConfigDocumentInTxn(opCtx, nss, request, txnNumber);
    };

    while (!docs.empty()) {
        BSONObj toAdd = docs.back();
        docs.pop_back();

        const int docSizePlusOverhead =
            toAdd.objsize() + write_ops::kRetryableAndTxnBatchWriteBSONSizeOverhead;
        // Check if pushing this object will exceed the batch size limit or the max object size
        if ((workingBatchItemSize + 1 > write_ops::kMaxWriteBatchSize) ||
            (workingBatchDocSize + docSizePlusOverhead > BSONObjMaxUserSize)) {
            doBatchInsert();

            workingBatch.clear();
            workingBatchItemSize = 0;
            workingBatchDocSize = 0;
        }

        workingBatch.push_back(toAdd);
        ++workingBatchItemSize;
        workingBatchDocSize += docSizePlusOverhead;
    }

    if (!workingBatch.empty())
        doBatchInsert();
}

void ShardingCatalogManager::withTransaction(
    OperationContext* opCtx,
    const NamespaceString& namespaceForInitialFind,
    unique_function<void(OperationContext*, TxnNumber)> func) {
    AlternativeSessionRegion asr(opCtx);
    auto* const client = asr.opCtx()->getClient();
    {
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    }
    asr.opCtx()->setAlwaysInterruptAtStepDownOrUp();
    AuthorizationSession::get(client)->grantInternalAuthorization(client);
    TxnNumber txnNumber = 0;

    auto guard = makeGuard([opCtx = asr.opCtx(), &txnNumber] {
        try {
            abortTransaction(opCtx, txnNumber);
        } catch (DBException& e) {
            LOGV2_WARNING(5192100,
                          "Failed to abort transaction in AlternativeSessionRegion",
                          "error"_attr = redact(e));
        }
    });

    size_t attempt = 1;
    while (true) {
        // Some ErrorCategory::Interruption errors are also considered transient transaction errors.
        // We don't attempt to enumerate them explicitly. Instead, we retry on all
        // ErrorCategory::Interruption errors (e.g. LockTimeout) and detect whether asr.opCtx() was
        // killed by explicitly checking if it has been interrupted.
        asr.opCtx()->checkForInterrupt();
        ++txnNumber;

        // We stop retrying on ErrorCategory::NotPrimaryError and ErrorCategory::ShutdownError
        // exceptions because it is expected for another attempt on this same server to keep
        // receiving that error.
        try {
            startTransactionWithNoopFind(asr.opCtx(), namespaceForInitialFind, txnNumber);
            func(asr.opCtx(), txnNumber);
        } catch (const ExceptionForCat<ErrorCategory::NotPrimaryError>&) {
            throw;
        } catch (const ExceptionForCat<ErrorCategory::ShutdownError>&) {
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

        auto [cmdStatus, wcStatus] = commitTransaction(asr.opCtx(), txnNumber);
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
        // commitTransaction() specifies {writeConcern: {w: "majority"}} without a wtimeout, so it
        // isn't expected to have a write concern error unless the primary is stepping down or
        // shutting down or asr.opCtx() is killed. We throw because all of those cases are terminal
        // for the caller running a local replica set transaction anyway.
        uassertStatusOK(wcStatus);

        guard.dismiss();
        return;
    }
}

}  // namespace mongo
