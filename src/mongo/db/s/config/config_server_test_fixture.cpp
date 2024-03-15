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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <memory>
#include <typeinfo>
#include <utility>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/config_shard_wrapper.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/database_version.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::assertGet;

using std::string;
using std::vector;
using unittest::assertGet;

namespace {
ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly);
}  // namespace

ConfigServerTestFixture::ConfigServerTestFixture(Options options, bool setUpMajorityReads)
    : ShardingMongoDTestFixture(std::move(options), setUpMajorityReads) {}

ConfigServerTestFixture::~ConfigServerTestFixture() = default;

void ConfigServerTestFixture::setUpAndInitializeConfigDb() {
    ConfigServerTestFixture::setUp();
    // Initialize the config database while we have exclusive access.
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

void ConfigServerTestFixture::setUp() {
    ShardingMongoDTestFixture::setUp();

    // TODO: SERVER-26919 set the flag on the mock repl coordinator just for the window where it
    // actually needs to bypass the op observer.
    replicationCoordinator()->alwaysAllowWrites(true);

    // Initialize sharding components as a config server.
    serverGlobalParams.clusterRole = {
        ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};

    // The catalog manager requires a special executor used for operations during addShard.
    auto specialNet(std::make_unique<executor::NetworkInterfaceMock>());
    _mockNetworkForAddShard = specialNet.get();

    auto specialExec(makeThreadPoolTestExecutor(std::move(specialNet)));
    _executorForAddShard = specialExec.get();

    _addShardNetworkTestEnv =
        std::make_unique<NetworkTestEnv>(_executorForAddShard, _mockNetworkForAddShard);
    auto configServerCatalogCacheLoader = std::make_unique<ConfigServerCatalogCacheLoader>();
    CatalogCacheLoader::set(getServiceContext(), std::move(configServerCatalogCacheLoader));

    uassertStatusOK(initializeGlobalShardingStateForMongodForTest(ConnectionString::forLocal()));

    auto shardLocal = Grid::get(getServiceContext())->shardRegistry()->createLocalConfigShard();
    ASSERT_EQ(typeid(*shardLocal).name(), typeid(ConfigShardWrapper).name());
    auto localCatalogClient = std::make_unique<ShardingCatalogClientImpl>(shardLocal);
    ShardingCatalogManager::create(getServiceContext(),
                                   std::move(specialExec),
                                   std::move(shardLocal),
                                   std::move(localCatalogClient));
}

void ConfigServerTestFixture::tearDown() {
    _addShardNetworkTestEnv = nullptr;
    _executorForAddShard = nullptr;
    _mockNetworkForAddShard = nullptr;

    ShardingCatalogManager::clearForTests(getServiceContext());

    ShardingMongoDTestFixture::tearDown();
}

std::unique_ptr<ShardingCatalogClient> ConfigServerTestFixture::makeShardingCatalogClient() {
    return std::make_unique<ShardingCatalogClientImpl>(nullptr /* overrideConfigShard */);
}

std::unique_ptr<BalancerConfiguration> ConfigServerTestFixture::makeBalancerConfiguration() {
    return std::make_unique<BalancerConfiguration>();
}

std::unique_ptr<ClusterCursorManager> ConfigServerTestFixture::makeClusterCursorManager() {
    return std::make_unique<ClusterCursorManager>(getServiceContext()->getPreciseClockSource());
}

executor::NetworkInterfaceMock* ConfigServerTestFixture::networkForAddShard() const {
    invariant(_mockNetworkForAddShard);
    return _mockNetworkForAddShard;
}

executor::TaskExecutor* ConfigServerTestFixture::executorForAddShard() const {
    invariant(_executorForAddShard);
    return _executorForAddShard;
}

void ConfigServerTestFixture::onCommandForAddShard(NetworkTestEnv::OnCommandFunction func) {
    _addShardNetworkTestEnv->onCommand(func);
}

std::shared_ptr<Shard> ConfigServerTestFixture::getConfigShard() const {
    return shardRegistry()->getConfigShard();
}

Status ConfigServerTestFixture::insertToConfigCollection(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         const BSONObj& doc) {
    auto insertResponse = getConfigShard()->runCommand(
        opCtx,
        kReadPref,
        ns.dbName(),
        [&]() {
            write_ops::InsertCommandRequest insertOp(ns);
            insertOp.setDocuments({doc});
            return insertOp.toBSON({});
        }(),
        Milliseconds(defaultConfigCommandTimeoutMS.load()),
        Shard::RetryPolicy::kNoRetry);

    BatchedCommandResponse batchResponse;
    auto status = Shard::CommandResponse::processBatchWriteResponse(insertResponse, &batchResponse);
    return status;
}

Status ConfigServerTestFixture::updateToConfigCollection(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         const BSONObj& query,
                                                         const BSONObj& update,
                                                         const bool upsert) {
    auto updateResponse = getConfigShard()->runCommand(
        opCtx,
        kReadPref,
        ns.dbName(),
        [&]() {
            write_ops::UpdateCommandRequest updateOp(ns);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
                entry.setUpsert(upsert);
                return entry;
            }()});
            return updateOp.toBSON({});
        }(),
        Milliseconds(defaultConfigCommandTimeoutMS.load()),
        Shard::RetryPolicy::kNoRetry);


    BatchedCommandResponse batchResponse;
    auto status = Shard::CommandResponse::processBatchWriteResponse(updateResponse, &batchResponse);
    return status;
}

Status ConfigServerTestFixture::deleteToConfigCollection(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         const BSONObj& doc,
                                                         const bool multi) {
    auto deleteResponse = getConfigShard()->runCommand(
        opCtx,
        kReadPref,
        ns.dbName(),
        [&]() {
            write_ops::DeleteCommandRequest deleteOp(ns);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(doc);
                entry.setMulti(multi);
                return entry;
            }()});
            return deleteOp.toBSON({});
        }(),
        Milliseconds(defaultConfigCommandTimeoutMS.load()),
        Shard::RetryPolicy::kNoRetry);


    BatchedCommandResponse batchResponse;
    auto status = Shard::CommandResponse::processBatchWriteResponse(deleteResponse, &batchResponse);
    return status;
}

StatusWith<BSONObj> ConfigServerTestFixture::findOneOnConfigCollection(OperationContext* opCtx,
                                                                       const NamespaceString& ns,
                                                                       const BSONObj& filter,
                                                                       const BSONObj& sort) {
    auto config = getConfigShard();
    invariant(config);

    auto findStatus = config->exhaustiveFindOnConfig(
        opCtx, kReadPref, repl::ReadConcernLevel::kMajorityReadConcern, ns, filter, sort, 1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto findResult = findStatus.getValue();
    if (findResult.docs.empty()) {
        return Status(ErrorCodes::NoMatchingDocument, "No document found");
    }

    invariant(findResult.docs.size() == 1);
    return findResult.docs.front().getOwned();
}

void ConfigServerTestFixture::setupShards(const std::vector<ShardType>& shards) {
    const NamespaceString shardNS(NamespaceString::kConfigsvrShardsNamespace);
    for (const auto& shard : shards) {
        ASSERT_OK(insertToConfigCollection(operationContext(), shardNS, shard.toBSON()));
    }
}

StatusWith<ShardType> ConfigServerTestFixture::getShardDoc(OperationContext* opCtx,
                                                           const std::string& shardId) {
    auto doc = findOneOnConfigCollection(
        opCtx, NamespaceString::kConfigsvrShardsNamespace, BSON(ShardType::name(shardId)));
    if (!doc.isOK()) {
        if (doc.getStatus() == ErrorCodes::NoMatchingDocument) {
            return {ErrorCodes::ShardNotFound,
                    str::stream() << "shard " << shardId << " does not exist"};
        }
        return doc.getStatus();
    }

    return ShardType::fromBSON(doc.getValue());
}

CollectionType ConfigServerTestFixture::setupCollection(
    const NamespaceString& nss,
    const KeyPattern& shardKey,
    const std::vector<ChunkType>& chunks,
    std::function<void(CollectionType& coll)> collectionCustomizer) {
    auto dbDoc = findOneOnConfigCollection(
        operationContext(),
        NamespaceString::kConfigDatabasesNamespace,
        BSON(DatabaseType::kDbNameFieldName << nss.db_forTest().toString()));
    if (!dbDoc.isOK()) {
        // If the database is not setup, choose the first available shard as primary to implicitly
        // create the db
        auto swShardDoc = findOneOnConfigCollection(
            operationContext(), NamespaceString::kConfigsvrShardsNamespace, BSONObj());
        invariant(swShardDoc.isOK(),
                  "At least one shard should be setup when initializing a collection");
        auto shard = uassertStatusOK(ShardType::fromBSON(swShardDoc.getValue()));
        setupDatabase(nss.dbName(), ShardId(shard.getName()));
    }

    CollectionType coll(nss,
                        chunks[0].getVersion().epoch(),
                        chunks[0].getVersion().getTimestamp(),
                        Date_t::now(),
                        chunks[0].getCollectionUUID(),
                        shardKey);
    collectionCustomizer(coll);

    ASSERT_OK(
        insertToConfigCollection(operationContext(), CollectionType::ConfigNS, coll.toBSON()));

    for (const auto& chunk : chunks) {
        ASSERT_OK(insertToConfigCollection(
            operationContext(), ChunkType::ConfigNS, chunk.toConfigBSON()));
    }

    return coll;
}

StatusWith<ChunkType> ConfigServerTestFixture::getChunkDoc(OperationContext* opCtx,
                                                           const UUID& uuid,
                                                           const BSONObj& minKey,
                                                           const OID& collEpoch,
                                                           const Timestamp& collTimestamp) {

    const auto query = BSON(ChunkType::collectionUUID() << uuid << ChunkType::min(minKey));
    auto doc = findOneOnConfigCollection(opCtx, ChunkType::ConfigNS, query);
    if (!doc.isOK())
        return doc.getStatus();

    return ChunkType::parseFromConfigBSON(doc.getValue(), collEpoch, collTimestamp);
}

StatusWith<ChunkType> ConfigServerTestFixture::getChunkDoc(OperationContext* opCtx,
                                                           const BSONObj& minKey,
                                                           const OID& collEpoch,
                                                           const Timestamp& collTimestamp) {
    auto doc = findOneOnConfigCollection(opCtx, ChunkType::ConfigNS, BSON(ChunkType::min(minKey)));
    if (!doc.isOK())
        return doc.getStatus();

    return ChunkType::parseFromConfigBSON(doc.getValue(), collEpoch, collTimestamp);
}

StatusWith<ChunkVersion> ConfigServerTestFixture::getCollectionPlacementVersion(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto collectionDoc = findOneOnConfigCollection(
        opCtx, CollectionType::ConfigNS, BSON(CollectionType::kNssFieldName << nss.ns_forTest()));
    if (!collectionDoc.isOK())
        return collectionDoc.getStatus();

    const CollectionType coll(collectionDoc.getValue());

    auto chunkDoc =
        findOneOnConfigCollection(opCtx,
                                  ChunkType::ConfigNS,
                                  BSON(ChunkType::collectionUUID << coll.getUuid()) /* query */,
                                  BSON(ChunkType::lastmod << -1) /* sort */);

    if (!chunkDoc.isOK())
        return chunkDoc.getStatus();

    const auto chunkType =
        ChunkType::parseFromConfigBSON(chunkDoc.getValue(), coll.getEpoch(), coll.getTimestamp());
    if (!chunkType.isOK()) {
        return chunkType.getStatus();
    }

    return chunkType.getValue().getVersion();
}

DatabaseType ConfigServerTestFixture::setupDatabase(const DatabaseName& dbName,
                                                    const ShardId& primaryShard,
                                                    const DatabaseVersion& dbVersion) {
    DatabaseType db(dbName, primaryShard, dbVersion);
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigDatabasesNamespace,
                                                    db.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    return db;
}

StatusWith<std::vector<BSONObj>> ConfigServerTestFixture::getIndexes(OperationContext* opCtx,
                                                                     const NamespaceString& ns) {
    auto configShard = getConfigShard();

    auto response = configShard->runCommand(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            ns.dbName(),
                                            BSON("listIndexes" << ns.coll().toString()),
                                            Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                            Shard::RetryPolicy::kIdempotent);
    if (!response.isOK()) {
        return response.getStatus();
    }
    if (!response.getValue().commandStatus.isOK()) {
        return response.getValue().commandStatus;
    }

    auto cursorResponse = CursorResponse::parseFromBSON(response.getValue().response);
    if (!cursorResponse.isOK()) {
        return cursorResponse.getStatus();
    }
    return cursorResponse.getValue().getBatch();
}

std::vector<KeysCollectionDocument> ConfigServerTestFixture::getKeys(OperationContext* opCtx) {
    auto config = getConfigShard();
    auto findStatus = config->exhaustiveFindOnConfig(opCtx,
                                                     kReadPref,
                                                     repl::ReadConcernLevel::kMajorityReadConcern,
                                                     NamespaceString::kKeysCollectionNamespace,
                                                     BSONObj(),
                                                     BSON("expiresAt" << 1),
                                                     boost::none);
    ASSERT_OK(findStatus.getStatus());

    std::vector<KeysCollectionDocument> keys;
    const auto& docs = findStatus.getValue().docs;
    for (const auto& doc : docs) {
        auto key = KeysCollectionDocument::parse(IDLParserContext("keyDoc"), doc);
        keys.push_back(std::move(key));
    }

    return keys;
}

void ConfigServerTestFixture::setupOpObservers() {
    auto opObserverRegistry =
        checked_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
    opObserverRegistry->addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserverRegistry->addObserver(std::make_unique<ConfigServerOpObserver>());
}

}  // namespace mongo
