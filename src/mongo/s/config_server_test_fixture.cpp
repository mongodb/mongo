/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/config_server_test_fixture.h"

#include <algorithm>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/replset_dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_local.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

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

ConfigServerTestFixture::ConfigServerTestFixture() = default;

ConfigServerTestFixture::~ConfigServerTestFixture() = default;

void ConfigServerTestFixture::setUp() {
    ShardingMongodTestFixture::setUp();

    // TODO: SERVER-26919 set the flag on the mock repl coordinator just for the window where it
    // actually needs to bypass the op observer.
    replicationCoordinator()->alwaysAllowWrites(true);

    // Initialize sharding components as a config server.
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;

    {
        // The catalog manager requires a special executor used for operations during addShard.
        auto specialNet(stdx::make_unique<executor::NetworkInterfaceMock>());
        _mockNetworkForAddShard = specialNet.get();

        auto specialExec(makeThreadPoolTestExecutor(std::move(specialNet)));
        _executorForAddShard = specialExec.get();

        ShardingCatalogManager::create(getServiceContext(), std::move(specialExec));
    }

    _addShardNetworkTestEnv =
        stdx::make_unique<NetworkTestEnv>(_executorForAddShard, _mockNetworkForAddShard);

    CatalogCacheLoader::set(getServiceContext(),
                            stdx::make_unique<ConfigServerCatalogCacheLoader>());

    uassertStatusOK(initializeGlobalShardingStateForMongodForTest(ConnectionString::forLocal()));
}

void ConfigServerTestFixture::tearDown() {
    _addShardNetworkTestEnv = nullptr;
    _executorForAddShard = nullptr;
    _mockNetworkForAddShard = nullptr;

    ShardingCatalogManager::clearForTests(getServiceContext());

    CatalogCacheLoader::clearForTests(getServiceContext());

    ShardingMongodTestFixture::tearDown();
}

std::unique_ptr<DistLockCatalog> ConfigServerTestFixture::makeDistLockCatalog() {
    return stdx::make_unique<DistLockCatalogImpl>();
}

std::unique_ptr<DistLockManager> ConfigServerTestFixture::makeDistLockManager(
    std::unique_ptr<DistLockCatalog> distLockCatalog) {
    invariant(distLockCatalog);
    return stdx::make_unique<ReplSetDistLockManager>(
        getServiceContext(),
        "distLockProcessId",
        std::move(distLockCatalog),
        ReplSetDistLockManager::kDistLockPingInterval,
        ReplSetDistLockManager::kDistLockExpirationTime);
}

std::unique_ptr<ShardingCatalogClient> ConfigServerTestFixture::makeShardingCatalogClient(
    std::unique_ptr<DistLockManager> distLockManager) {
    invariant(distLockManager);
    return stdx::make_unique<ShardingCatalogClientImpl>(std::move(distLockManager));
}

std::unique_ptr<BalancerConfiguration> ConfigServerTestFixture::makeBalancerConfiguration() {
    return stdx::make_unique<BalancerConfiguration>();
}

std::unique_ptr<ClusterCursorManager> ConfigServerTestFixture::makeClusterCursorManager() {
    return stdx::make_unique<ClusterCursorManager>(getServiceContext()->getPreciseClockSource());
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
    auto insertResponse = getConfigShard()->runCommand(opCtx,
                                                       kReadPref,
                                                       ns.db().toString(),
                                                       [&]() {
                                                           write_ops::Insert insertOp(ns);
                                                           insertOp.setDocuments({doc});
                                                           return insertOp.toBSON({});
                                                       }(),
                                                       Shard::kDefaultConfigCommandTimeout,
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
    auto updateResponse = getConfigShard()->runCommand(opCtx,
                                                       kReadPref,
                                                       ns.db().toString(),
                                                       [&]() {
                                                           write_ops::Update updateOp(ns);
                                                           updateOp.setUpdates({[&] {
                                                               write_ops::UpdateOpEntry entry;
                                                               entry.setQ(query);
                                                               entry.setU(update);
                                                               entry.setUpsert(upsert);
                                                               return entry;
                                                           }()});
                                                           return updateOp.toBSON({});
                                                       }(),
                                                       Shard::kDefaultConfigCommandTimeout,
                                                       Shard::RetryPolicy::kNoRetry);


    BatchedCommandResponse batchResponse;
    auto status = Shard::CommandResponse::processBatchWriteResponse(updateResponse, &batchResponse);
    return status;
}

Status ConfigServerTestFixture::deleteToConfigCollection(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         const BSONObj& doc,
                                                         const bool multi) {
    auto deleteResponse = getConfigShard()->runCommand(opCtx,
                                                       kReadPref,
                                                       ns.db().toString(),
                                                       [&]() {
                                                           write_ops::Delete deleteOp(ns);
                                                           deleteOp.setDeletes({[&] {
                                                               write_ops::DeleteOpEntry entry;
                                                               entry.setQ(doc);
                                                               entry.setMulti(multi);
                                                               return entry;
                                                           }()});
                                                           return deleteOp.toBSON({});
                                                       }(),
                                                       Shard::kDefaultConfigCommandTimeout,
                                                       Shard::RetryPolicy::kNoRetry);


    BatchedCommandResponse batchResponse;
    auto status = Shard::CommandResponse::processBatchWriteResponse(deleteResponse, &batchResponse);
    return status;
}

StatusWith<BSONObj> ConfigServerTestFixture::findOneOnConfigCollection(OperationContext* opCtx,
                                                                       const NamespaceString& ns,
                                                                       const BSONObj& filter) {
    auto config = getConfigShard();
    invariant(config);

    auto findStatus = config->exhaustiveFindOnConfig(
        opCtx, kReadPref, repl::ReadConcernLevel::kMajorityReadConcern, ns, filter, BSONObj(), 1);
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

Status ConfigServerTestFixture::setupShards(const std::vector<ShardType>& shards) {
    const NamespaceString shardNS(ShardType::ConfigNS);
    for (const auto& shard : shards) {
        auto insertStatus = insertToConfigCollection(operationContext(), shardNS, shard.toBSON());
        if (!insertStatus.isOK()) {
            return insertStatus;
        }
    }

    return Status::OK();
}

StatusWith<ShardType> ConfigServerTestFixture::getShardDoc(OperationContext* opCtx,
                                                           const std::string& shardId) {
    auto doc =
        findOneOnConfigCollection(opCtx, ShardType::ConfigNS, BSON(ShardType::name(shardId)));
    if (!doc.isOK()) {
        if (doc.getStatus() == ErrorCodes::NoMatchingDocument) {
            return {ErrorCodes::ShardNotFound,
                    str::stream() << "shard " << shardId << " does not exist"};
        }
        return doc.getStatus();
    }

    return ShardType::fromBSON(doc.getValue());
}

Status ConfigServerTestFixture::setupChunks(const std::vector<ChunkType>& chunks) {
    const NamespaceString chunkNS(ChunkType::ConfigNS);
    for (const auto& chunk : chunks) {
        auto insertStatus =
            insertToConfigCollection(operationContext(), chunkNS, chunk.toConfigBSON());
        if (!insertStatus.isOK())
            return insertStatus;
    }

    return Status::OK();
}

StatusWith<ChunkType> ConfigServerTestFixture::getChunkDoc(OperationContext* opCtx,
                                                           const BSONObj& minKey) {
    auto doc =
        findOneOnConfigCollection(opCtx, ChunkType::ConfigNS, BSON(ChunkType::min() << minKey));
    if (!doc.isOK())
        return doc.getStatus();

    return ChunkType::fromConfigBSON(doc.getValue());
}

void ConfigServerTestFixture::setupDatabase(const std::string& dbName,
                                            const ShardId primaryShard,
                                            const bool sharded) {
    DatabaseType db(dbName, primaryShard, sharded, databaseVersion::makeNew());
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    DatabaseType::ConfigNS,
                                                    db.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
}

StatusWith<std::vector<BSONObj>> ConfigServerTestFixture::getIndexes(OperationContext* opCtx,
                                                                     const NamespaceString& ns) {
    auto configShard = getConfigShard();

    auto response = configShard->runCommand(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            ns.db().toString(),
                                            BSON("listIndexes" << ns.coll().toString()),
                                            Shard::kDefaultConfigCommandTimeout,
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
                                                     KeysCollectionDocument::ConfigNS,
                                                     BSONObj(),
                                                     BSON("expiresAt" << 1),
                                                     boost::none);
    ASSERT_OK(findStatus.getStatus());

    std::vector<KeysCollectionDocument> keys;
    const auto& docs = findStatus.getValue().docs;
    for (const auto& doc : docs) {
        auto keyStatus = KeysCollectionDocument::fromBSON(doc);
        ASSERT_OK(keyStatus.getStatus());
        keys.push_back(keyStatus.getValue());
    }

    return keys;
}

void ConfigServerTestFixture::expectSetShardVersion(
    const HostAndPort& expectedHost,
    const ShardType& expectedShard,
    const NamespaceString& expectedNs,
    boost::optional<ChunkVersion> expectedChunkVersion) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(expectedHost, request.target);
        ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        SetShardVersionRequest ssv =
            assertGet(SetShardVersionRequest::parseFromBSON(request.cmdObj));

        ASSERT(!ssv.isInit());
        ASSERT(ssv.isAuthoritative());
        ASSERT_EQ(expectedShard.getHost(), ssv.getShardConnectionString().toString());
        ASSERT_EQ(expectedNs.toString(), ssv.getNS().ns());

        if (expectedChunkVersion) {
            ASSERT_EQ(*expectedChunkVersion, ssv.getNSVersion());
        }

        return BSON("ok" << true);
    });
}

}  // namespace mongo
