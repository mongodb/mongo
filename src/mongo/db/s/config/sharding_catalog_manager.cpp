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

#include "mongo/db/ops/write_ops_parsers.h"

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <algorithm>
#include <tuple>
#include <vector>

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/db/s/config/index_on_config.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/s/type_lockpings.h"
#include "mongo/db/s/type_locks.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
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
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log_and_backoff.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(shardingCatalogManagerWithTransactionFailWCAfterCommit);

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

/**
 * Runs the BatchedCommandRequest 'request' on namespace 'nss' It transforms the request to BSON
 * and then uses a DBDirectClient to run the command locally.
 */
BSONObj executeConfigRequest(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BatchedCommandRequest& request) {
    invariant(nss.db() == NamespaceString::kConfigDb);
    DBDirectClient client(opCtx);
    BSONObj result;
    client.runCommand(nss.db().toString(), request.toBSON(), result);
    return result;
}

void startTransactionWithNoopFind(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  TxnNumber txnNumber) {
    FindCommandRequest findCommand(nss);
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
                                 std::string cmdName,
                                 const WriteConcernOptions& writeConcern) {
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
    newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    AuthorizationSession::get(newOpCtx.get()->getClient())
        ->grantInternalAuthorization(newOpCtx.get()->getClient());
    newOpCtx.get()->setLogicalSessionId(opCtx->getLogicalSessionId().get());
    newOpCtx.get()->setTxnNumber(txnNumber);

    BSONObjBuilder bob;
    bob.append(cmdName, true);
    bob.append("autocommit", false);
    bob.append(OperationSessionInfo::kTxnNumberFieldName, txnNumber);
    bob.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());

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
        ChunkType::ConfigNS,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_min_1 index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_shard_1_min_1 index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::collectionUUID() << 1 << ChunkType::lastmod() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create uuid_1_lastmod_1 index on config db");
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

    Status status = _initConfigCollections(opCtx);
    if (!status.isOK()) {
        return status;
    }

    status = _initConfigIndexes(opCtx);
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

    Status result = createIndexesForConfigChunks(opCtx);
    if (result != Status::OK()) {
        return result;
    }

    result =
        createIndexOnConfigCollection(opCtx,
                                      MigrationType::ConfigNS,
                                      BSON(MigrationType::ns() << 1 << MigrationType::min() << 1),
                                      unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config.migrations");
    }

    result = createIndexOnConfigCollection(
        opCtx, NamespaceString::kConfigsvrShardsNamespace, BSON(ShardType::host() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create host_1 index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx, LocksType::ConfigNS, BSON(LocksType::lockID() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create lock id index on config db");
    }

    result =
        createIndexOnConfigCollection(opCtx,
                                      LocksType::ConfigNS,
                                      BSON(LocksType::state() << 1 << LocksType::process() << 1),
                                      !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create state and process id index on config db");
    }

    result = createIndexOnConfigCollection(
        opCtx, LockpingsType::ConfigNS, BSON(LockpingsType::ping() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create lockping ping time index on config db");
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

    if (feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        result = sharding_util::createGlobalIndexesIndexes(opCtx);
        if (!result.isOK()) {
            return result;
        }
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
    const bool ok = client.runCommand(CollectionType::ConfigNS.db().toString(), cmd, result);
    if (!ok) {  // create returns error NamespaceExists if collection already exists
        Status status = getStatusFromCommandResult(result);
        if (status != ErrorCodes::NamespaceExists) {
            return status.withContext("Could not create config.collections");
        }
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

    uassertStatusOK(getStatusFromWriteCommandReply(response));

    return response;
}

void ShardingCatalogManager::insertConfigDocuments(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   std::vector<BSONObj> docs,
                                                   boost::optional<TxnNumber> txnNumber) {
    invariant(nss.db() == NamespaceString::kConfigDb);

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
            writeToConfigDocumentInTxn(opCtx, nss, request, txnNumber.get());
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

    invariant(nss.db() == NamespaceString::kConfigDb);

    FindCommandRequest findCommand(nss);
    findCommand.setFilter(query);
    findCommand.setSingleBatch(true);
    findCommand.setLimit(1);

    auto res =
        runCommandInLocalTxn(
            opCtx, nss.db(), false /*startTransaction*/, txnNumber, findCommand.toBSON(BSONObj()))
            .body;
    uassertStatusOK(getStatusFromCommandResult(res));

    auto cursor = uassertStatusOK(CursorResponse::parseFromBSON(res));
    auto result = cursor.releaseBatch();

    if (result.empty()) {
        return boost::none;
    }

    return result.front().getOwned();
}

void ShardingCatalogManager::withTransactionAPI(OperationContext* opCtx,
                                                const NamespaceString& namespaceForInitialFind,
                                                txn_api::Callback callback) {
    auto txn =
        txn_api::SyncTransactionWithRetries(opCtx,
                                            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                                            nullptr /* resourceYielder */);
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
    withTransaction(opCtx,
                    namespaceForInitialFind,
                    std::move(func),
                    ShardingCatalogClient::kMajorityWriteConcern);
}

void ShardingCatalogManager::withTransaction(
    OperationContext* opCtx,
    const NamespaceString& namespaceForInitialFind,
    unique_function<void(OperationContext*, TxnNumber)> func,
    const WriteConcernOptions& writeConcern) {

    AlternativeSessionRegion asr(opCtx);
    auto* const client = asr.opCtx()->getClient();
    {
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    }
    asr.opCtx()->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    AuthorizationSession::get(client)->grantInternalAuthorization(client);
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
        // Some ErrorCategory::Interruption errors are also considered transient transaction
        // errors. We don't attempt to enumerate them explicitly. Instead, we retry on all
        // ErrorCategory::Interruption errors (e.g. LockTimeout) and detect whether asr.opCtx()
        // was killed by explicitly checking if it has been interrupted.
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
