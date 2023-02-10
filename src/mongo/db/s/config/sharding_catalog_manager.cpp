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

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <algorithm>
#include <tuple>
#include <vector>

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/db/s/config/index_on_config.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
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
    client.runCommand(nss.dbName(), request.toBSON(), result);
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
    {
        auto lk = stdx::lock_guard(*newOpCtx->getClient());
        newOpCtx->setLogicalSessionId(opCtx->getLogicalSessionId().value());
        newOpCtx->setTxnNumber(txnNumber);
    }

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

Status createIndexForConfigPlacementHistory(OperationContext* opCtx, bool waitForMajority) {
    auto status = createIndexOnConfigCollection(
        opCtx,
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        BSON(NamespacePlacementType::kNssFieldName
             << 1 << NamespacePlacementType::kTimestampFieldName << -1),
        true /*unique*/);
    if (status.isOK() && waitForMajority) {
        auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
        WriteConcernResult unusedResult;
        status = waitForWriteConcern(opCtx,
                                     replClient.getLastOp(),
                                     ShardingCatalogClient::kMajorityWriteConcern,
                                     &unusedResult);
    }
    return status;
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

class PipelineBuilder {

public:
    PipelineBuilder(OperationContext* opCtx,
                    const NamespaceString& nss,
                    std::vector<NamespaceString>&& resolvedNamespaces)
        : _expCtx{make_intrusive<ExpressionContext>(opCtx, nullptr /*collator*/, nss)} {

        StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespacesMap;

        for (const auto& collNs : resolvedNamespaces) {
            resolvedNamespacesMap[collNs.coll()] = {collNs, std::vector<BSONObj>() /* pipeline */};
        }

        _expCtx->setResolvedNamespaces(resolvedNamespacesMap);
    }

    PipelineBuilder(const boost::intrusive_ptr<ExpressionContext>& expCtx) : _expCtx(expCtx) {}

    template <typename T>
    PipelineBuilder& addStage(mongo::BSONObj&& bsonObj) {
        _stages.emplace_back(_toStage<T>(_expCtx, std::move(bsonObj)));
        return *this;
    }

    std::vector<BSONObj> toBson() {
        return build()->serializeToBson();
    }

    AggregateCommandRequest toAggregateCommandRequest() {
        return AggregateCommandRequest(_expCtx->ns, toBson());
    }

    std::unique_ptr<Pipeline, PipelineDeleter> build() {
        return Pipeline::create(std::move(_stages), _expCtx);
    }

    boost::intrusive_ptr<ExpressionContext>& getExpCtx() {
        return _expCtx;
    }

private:
    template <typename T>
    boost::intrusive_ptr<DocumentSource> _toStage(boost::intrusive_ptr<ExpressionContext>& expCtx,
                                                  mongo::BSONObj&& bsonObj) {
        return T::createFromBson(Document{{T::kStageName, bsonObj}}.toBson().firstElement(),
                                 expCtx);
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    Pipeline::SourceContainer _stages;
};

AggregateCommandRequest createInitPlacementHistoryAggregationRequest(
    OperationContext* opCtx, const Timestamp& initTimestamp) {
    // Compose the pipeline to generate a NamespacePlacementType for every collection and database
    // in the cluster.
    // 1. Join config.collections and config.chunks using the collection UUID. This will create
    // an array of docs called chunk_docs that contains all the chunk entries for the
    // collection. As a part of the join, run a pipeline on every chunk_docs that pushes the
    // chunk.shard into a a set. This will create a field called shardsSet that contains a
    // unique set of shards for the collection.
    // 2. Translate the output to a config.placementHistory entry format.
    // 3. Concatenate the result of the join with config.databases and convert the output to a
    // config.placementHistory entry format.
    // 4. Insert the result into config.placementHistory.
    /* config.collections.aggregate([
    {
        "$lookup": {
        "from": "chunks",
        "localField": "uuid",
        "foreignField": "uuid",
        "as": "chunk_docs",
        "pipeline":
        [
            {
                "$project":
                {
                    "shard": 1
                }
            },
            {
                "$group":
                {
                    _id: "",
                    shardsSet:
                    {
                        $addToSet: "$shard"
                    }
                }
            }
        ]
        }
    },
    {
        $unwind: {
            path: "$chunk_docs"
        }
    },
    {
        "$project":
        {
            "_id": 0,
            "nss": "$_id",
            "shards": "$chunk_docs.shardsSet"
        }
    },
    {
        "$unionWith":
        {
            "coll": "databases",
            "pipeline":
            [
                {
                    "$project":
                    {
                        "_id": 0,
                        "nss": "$_id",
                        "shards": [ "$primary"]
                    }
                }
            ]
         }

    },
    {
        "$set"
        {
            "timestamp": "<configTimeOfSnapshotRead>"
        }
    }
    //insertion to placementHistory
    {
        "$merge":
        {
            "into": "config.placementHistory",
            "on": ["nss", "timestamp"],
            "whenMatched": "replace",
            "whenNotMatched": "insert"
        }
    }
])
*/
    using Lookup = DocumentSourceLookUp;
    using UnionWith = DocumentSourceUnionWith;
    using Merge = DocumentSourceMerge;
    using Set = DocumentSourceAddFields;
    using Group = DocumentSourceGroup;
    using Project = DocumentSourceProject;
    using Unwind = DocumentSourceUnwind;

    const auto kNss = NamespacePlacementType::kNssFieldName.toString();
    const auto kUuid = NamespacePlacementType::kUuidFieldName.toString();
    const auto kShards = NamespacePlacementType::kShardsFieldName.toString();
    const auto kTimestamp = NamespacePlacementType::kTimestampFieldName.toString();
    const auto kUuidChunks = ChunkType::collectionUUID.name();

    auto pipeline = PipelineBuilder(opCtx,
                                    CollectionType::ConfigNS,
                                    {ChunkType::ConfigNS,
                                     CollectionType::ConfigNS,
                                     NamespaceString::kConfigDatabasesNamespace,
                                     NamespaceString::kConfigsvrPlacementHistoryNamespace});

    // Stage 1. Join config.collections and config.chunks using the collection UUID and create a set
    // of shards
    pipeline.addStage<Lookup>(
        BSON("from" << ChunkType::ConfigNS.coll() << "localField" << CollectionType::kUuidFieldName
                    << "foreignField" << ChunkType::collectionUUID.name() << "as"
                    << "chunk_docs"
                    << "pipeline"
                    << PipelineBuilder(pipeline.getExpCtx())
                           // Stage 1. Project the shard field
                           .addStage<Project>(BSON(ChunkType::shard << 1))
                           // Stage 2. Group by shard and push the document's shard field
                           // into an array called "shards"
                           .addStage<Group>(BSON("_id"
                                                 << ""
                                                 << "shardSet"
                                                 << BSON("$addToSet"
                                                         << "$" + ChunkType::shard.name())))
                           .toBson()));

    // Stage 2. Unwind the chunk_docs array (which after the pipeline is of size 1)
    pipeline.addStage<Unwind>(BSON("path"
                                   << "$chunk_docs"));

    // Stage 3. Adapt the output to the config.placementHistory schema
    pipeline.addStage<Project>(BSON("_id" << 0 << kShards << "$chunk_docs.shardSet" << kNss
                                          << "$_id" << kUuid << "$" + kUuidChunks));

    // Stage 4. Union with the databases collection
    pipeline.addStage<UnionWith>(
        BSON("coll" << NamespaceString::kConfigDatabasesNamespace.coll() <<
             // unionWith pipeline to convert the output to a placementHistory entry format
             "pipeline"
                    << PipelineBuilder(pipeline.getExpCtx())
                           .addStage<DocumentSourceProject>(BSON(
                               "_id" << 0 << kNss << "$_id" << kShards << BSON_ARRAY("$primary")))
                           .toBson()));

    // Stage 5. Set the timestamp
    pipeline.addStage<Set>(BSON(kTimestamp << initTimestamp));

    // Stage 6. Merge into the placementHistory collection
    pipeline.addStage<Merge>(BSON("into"
                                  << NamespaceString::kConfigsvrPlacementHistoryNamespace.coll()
                                  << "on" << BSON_ARRAY(kNss << kTimestamp) << "whenMatched"
                                  << "replace"
                                  << "whenNotMatched"
                                  << "insert"));

    return pipeline.toAggregateCommandRequest();
}

}  // namespace

void ShardingCatalogManager::create(ServiceContext* serviceContext,
                                    std::unique_ptr<executor::TaskExecutor> addShardExecutor,
                                    std::shared_ptr<Shard> localConfigShard,
                                    std::unique_ptr<ShardingCatalogClient> localCatalogClient) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

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
    std::unique_ptr<executor::TaskExecutor> addShardExecutor,
    std::shared_ptr<Shard> localConfigShard,
    std::unique_ptr<ShardingCatalogClient> localCatalogClient)
    : _serviceContext(serviceContext),
      _executorForAddShard(std::move(addShardExecutor)),
      _localConfigShard(std::move(localConfigShard)),
      _localCatalogClient(std::move(localCatalogClient)),
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

    if (feature_flags::gConfigSettingsSchema.isEnabled(serverGlobalParams.featureCompatibility)) {
        status = _initConfigSettings(opCtx);
        if (!status.isOK()) {
            return status;
        }
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

Status ShardingCatalogManager::upgradeConfigSettings(OperationContext* opCtx) {
    return _initConfigSettings(opCtx);
}

ShardingCatalogClient* ShardingCatalogManager::localCatalogClient() {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
    return _localCatalogClient.get();
}

void ShardingCatalogManager::discardCachedConfigDatabaseInitializationState() {
    stdx::lock_guard<Latch> lk(_mutex);
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

    if (!feature_flags::gStopUsingConfigVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        newVersion.setCurrentVersion(VersionType::CURRENT_CONFIG_VERSION);
        newVersion.setMinCompatibleVersion(VersionType::MIN_COMPATIBLE_CONFIG_VERSION);
    }
    auto insertStatus = _localCatalogClient->insertConfigDocument(
        opCtx, VersionType::ConfigNS, newVersion.toBSON(), kNoWaitWriteConcern);
    return insertStatus;
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

    if (feature_flags::gHistoricalPlacementShardingCatalog.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        result = createIndexForConfigPlacementHistory(opCtx, false /*waitForMajority*/);
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

    /**
     * $jsonSchema: {
     *   oneOf: [
     *       {"properties": {_id: {enum: ["chunksize"]}},
     *                      {value: {bsonType: "number", minimum: 1, maximum: 1024}}},
     *       {"properties": {_id: {enum: ["balancer", "autosplit", "ReadWriteConcernDefaults",
     *                                    "audit"]}}}
     *   ]
     * }
     *
     * Note: the schema uses "number" for the chunksize instead of "int" because "int" requires the
     * user to pass NumberInt(x) as the value rather than x (as all of our docs recommend). Non-
     * integer values will be handled as they were before the schema, by the balancer failing until
     * a new value is set.
     */
    const auto chunkSizeValidator =
        BSON("properties" << BSON("_id" << BSON("enum" << BSON_ARRAY(ChunkSizeSettingsType::kKey))
                                        << "value"
                                        << BSON("bsonType"
                                                << "number"
                                                << "minimum" << 1 << "maximum" << 1024))
                          << "additionalProperties" << false);
    const auto noopValidator =
        BSON("properties" << BSON(
                 "_id" << BSON("enum" << BSON_ARRAY(
                                   BalancerSettingsType::kKey
                                   << AutoSplitSettingsType::kKey << AutoMergeSettingsType::kKey
                                   << ReadWriteConcernDefaults::kPersistedDocumentId << "audit"))));
    const auto fullValidator =
        BSON("$jsonSchema" << BSON("oneOf" << BSON_ARRAY(chunkSizeValidator << noopValidator)));

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
        opCtx, {NamespaceString::kConfigSettingsNamespace}, collModCmd, &builder);
}

Status ShardingCatalogManager::setFeatureCompatibilityVersionOnShards(OperationContext* opCtx,
                                                                      const BSONObj& cmdObj) {

    // No shards should be added until we have forwarded featureCompatibilityVersion to all shards.
    Lock::SharedLock lk(opCtx, _kShardMembershipLock);

    // We do a direct read of the shards collection with local readConcern so no shards are missed,
    // but don't go through the ShardRegistry to prevent it from caching data that may be rolled
    // back.
    const auto opTimeWithShards = uassertStatusOK(
        _localCatalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern));

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


void ShardingCatalogManager::initializePlacementHistory(OperationContext* opCtx) {
    uassertStatusOK(createIndexForConfigPlacementHistory(opCtx, true /*waitForMajority*/));

    // Building a initial state/snapshot for the placementHistory.
    // The initial state is composed by a set of entries: one for each collection and one for each
    // database. Since the initial snapshot represents the current state of the cluster, the
    // timestamp of all the entries is the same. Any other migration or ddl happening in the
    // background could change the catalog. We don't need to serialize the insert in the
    // placementhistory since we can order the entries by timestamp.
    const auto now = VectorClock::get(opCtx)->getTime();
    const auto initTime = now.configTime().asTimestamp();

    // We need to perform this operation with internal permissions to add an aggregation
    // $merge stage targeting the catalog.
    const bool wasInternalClient = isInternalClient(opCtx->getClient());
    if (!wasInternalClient) {
        opCtx->getClient()->session()->setTags(transport::Session::kInternalClient);
    }

    // We must reset the internal flag.
    ON_BLOCK_EXIT([&] {
        if (!wasInternalClient) {
            opCtx->getClient()->session()->unsetTags(transport::Session::kInternalClient);
        }
    });

    auto aggRequest = createInitPlacementHistoryAggregationRequest(opCtx, initTime);
    aggRequest.setUnwrappedReadPref({});
    repl::ReadConcernArgs readConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
    readConcernArgs.setArgsAtClusterTimeForSnapshot(initTime);
    aggRequest.setReadConcern(readConcernArgs.toBSONInner());
    aggRequest.setWriteConcern({});

    // no-op callback
    auto callback = [](const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken) {
        return true;
    };

    const Status status = _localConfigShard->runAggregation(opCtx, aggRequest, callback);
    uassertStatusOK(status);
}

}  // namespace mongo
