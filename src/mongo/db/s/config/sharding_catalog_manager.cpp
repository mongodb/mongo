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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"

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
            .response);
}

void insertDocumentsInLocalTxn(OperationContext* opCtx,
                               const NamespaceString& nss,
                               std::vector<BSONObj> docs,
                               bool startTransaction,
                               TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments(std::move(docs));
        return insertOp;
    }());

    uassertStatusOK(getStatusFromWriteCommandReply(
        runCommandInLocalTxn(opCtx, nss.db(), startTransaction, txnNumber, request.toBSON()).body));
}

void removeDocumentsInLocalTxn(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& query,
                               bool startTransaction,
                               TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());

    uassertStatusOK(getStatusFromWriteCommandReply(
        runCommandInLocalTxn(opCtx, nss.db(), startTransaction, txnNumber, request.toBSON()).body));
}

void commitLocalTxn(OperationContext* opCtx, TxnNumber txnNumber) {
    uassertStatusOK(
        getStatusFromCommandResult(runCommandInLocalTxn(opCtx,
                                                        NamespaceString::kAdminDb,
                                                        false /* startTransaction */,
                                                        txnNumber,
                                                        BSON(CommitTransaction::kCommandName << 1))
                                       .body));
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
      _kZoneOpLock("zoneOpLock"),
      _kChunkOpLock("chunkOpLock"),
      _kShardMembershipLock("shardMembershipLock") {
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
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _inShutdown = true;
    }

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

    result = configShard->createIndexOnConfig(
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

Lock::ExclusiveLock ShardingCatalogManager::lockZoneMutex(OperationContext* opCtx) {
    Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);
    return lk;
}

// TODO SERVER-44034: Remove this function.
void deleteAndInsertChunk(OperationContext* opCtx,
                          const BSONObj& chunkDoc,
                          bool startTransaction,
                          TxnNumber txnNumber,
                          ShardingCatalogManager::ConfigUpgradeType upgradeType) {
    auto chunk = uassertStatusOK(ChunkType::fromConfigBSON(chunkDoc));

    removeDocumentsInLocalTxn(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::ns(chunk.getNS().ns()) << ChunkType::min(chunk.getMin())),
        startTransaction,
        txnNumber);

    insertDocumentsInLocalTxn(
        opCtx,
        ChunkType::ConfigNS,
        {upgradeType == ShardingCatalogManager::ConfigUpgradeType::kUpgrade
             // Note that ChunkType::toConfigBSON() will not include an _id if one hasn't been set,
             // which will be the case for chunks written in the 4.2 format because parsing ignores
             // _ids in the 4.2 format, so the insert path will generate one for us.
             ? chunk.toConfigBSON()
             : chunk.toConfigBSONLegacyID()},
        false /* startTransaction */,
        txnNumber);
}

// TODO SERVER-44034: Remove this function.
void deleteAndInsertTag(OperationContext* opCtx,
                        const BSONObj& tagDoc,
                        bool startTransaction,
                        TxnNumber txnNumber,
                        ShardingCatalogManager::ConfigUpgradeType upgradeType) {
    auto tag = uassertStatusOK(TagsType::fromBSON(tagDoc));

    removeDocumentsInLocalTxn(
        opCtx,
        TagsType::ConfigNS,
        BSON(TagsType::ns(tag.getNS().ns()) << TagsType::min(tag.getMinKey())),
        startTransaction,
        txnNumber);

    insertDocumentsInLocalTxn(opCtx,
                              TagsType::ConfigNS,
                              {upgradeType == ShardingCatalogManager::ConfigUpgradeType::kUpgrade
                                   // Note that TagsType::toBSON() will not include an _id, so the
                                   // insert path will generate one for us.
                                   ? tag.toBSON()
                                   : tag.toBSONLegacyID()},
                              false /* startTransaction */,
                              txnNumber);
}

// TODO SERVER-44034: Remove this function and type.
using ConfigDocModFunction = std::function<void(
    OperationContext*, BSONObj, bool, TxnNumber, ShardingCatalogManager::ConfigUpgradeType)>;
void forEachConfigDocInBatchedTransactions(OperationContext* opCtx,
                                           const NamespaceString& configNss,
                                           const NamespaceString& shardedCollNss,
                                           ConfigDocModFunction configDocModFn,
                                           ShardingCatalogManager::ConfigUpgradeType upgradeType) {
    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            configNss,
                                            BSON("ns" << shardedCollNss.ns()),
                                            {},
                                            boost::none /* limit */));

    AlternativeSessionRegion asr(opCtx);
    AuthorizationSession::get(asr.opCtx()->getClient())
        ->grantInternalAuthorization(asr.opCtx()->getClient());
    TxnNumber txnNumber = 0;

    const auto batchSizeLimit = 100;
    auto currentBatchSize = 0;
    for (const auto& doc : findResponse.docs) {
        auto startTransaction = currentBatchSize == 0;

        configDocModFn(asr.opCtx(), doc, startTransaction, txnNumber, upgradeType);

        currentBatchSize += 1;
        if (currentBatchSize == batchSizeLimit) {
            commitLocalTxn(asr.opCtx(), txnNumber);
            txnNumber += 1;
            currentBatchSize = 0;
        }
    }

    if (currentBatchSize != 0) {
        commitLocalTxn(asr.opCtx(), txnNumber);
    }
}

void ShardingCatalogManager::upgradeOrDowngradeChunksAndTags(OperationContext* opCtx,
                                                             ConfigUpgradeType upgradeType) {
    const auto grid = Grid::get(opCtx);
    auto allDbs = uassertStatusOK(grid->catalogClient()->getAllDBs(
                                      opCtx, repl::ReadConcernLevel::kLocalReadConcern))
                      .value;

    // The 'config' database contains the sharded 'config.system.sessions' collection but does not
    // have an entry in config.databases.
    allDbs.emplace_back("config", ShardId("config"), true, DatabaseVersion());

    for (const auto& db : allDbs) {
        auto collections = uassertStatusOK(grid->catalogClient()->getCollections(
            opCtx, &db.getName(), nullptr, repl::ReadConcernLevel::kLocalReadConcern));

        for (const auto& coll : collections) {
            if (coll.getDropped()) {
                continue;
            }

            {
                Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);
                forEachConfigDocInBatchedTransactions(
                    opCtx, ChunkType::ConfigNS, coll.getNs(), deleteAndInsertChunk, upgradeType);
            }

            {
                Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);
                forEachConfigDocInBatchedTransactions(
                    opCtx, TagsType::ConfigNS, coll.getNs(), deleteAndInsertTag, upgradeType);
            }
        }
    }
}

}  // namespace mongo
