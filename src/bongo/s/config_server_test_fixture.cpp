/**
 *    Copyright (C) 2016 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/s/config_server_test_fixture.h"

#include <algorithm>
#include <vector>

#include "bongo/base/status_with.h"
#include "bongo/client/remote_command_targeter_factory_mock.h"
#include "bongo/client/remote_command_targeter_mock.h"
#include "bongo/db/client.h"
#include "bongo/db/commands.h"
#include "bongo/db/db_raii.h"
#include "bongo/db/namespace_string.h"
#include "bongo/db/op_observer.h"
#include "bongo/db/query/cursor_response.h"
#include "bongo/db/query/query_request.h"
#include "bongo/db/repl/oplog.h"
#include "bongo/db/repl/read_concern_args.h"
#include "bongo/db/repl/repl_settings.h"
#include "bongo/db/repl/replication_coordinator_mock.h"
#include "bongo/executor/network_interface_mock.h"
#include "bongo/executor/task_executor_pool.h"
#include "bongo/executor/thread_pool_task_executor_test_fixture.h"
#include "bongo/rpc/metadata/repl_set_metadata.h"
#include "bongo/rpc/metadata/server_selection_metadata.h"
#include "bongo/s/balancer_configuration.h"
#include "bongo/s/catalog/dist_lock_catalog_impl.h"
#include "bongo/s/catalog/replset_dist_lock_manager.h"
#include "bongo/s/catalog/sharding_catalog_client_impl.h"
#include "bongo/s/catalog/sharding_catalog_manager_impl.h"
#include "bongo/s/catalog/type_changelog.h"
#include "bongo/s/catalog/type_chunk.h"
#include "bongo/s/catalog/type_collection.h"
#include "bongo/s/catalog/type_shard.h"
#include "bongo/s/catalog_cache.h"
#include "bongo/s/client/shard_factory.h"
#include "bongo/s/client/shard_local.h"
#include "bongo/s/client/shard_registry.h"
#include "bongo/s/client/shard_remote.h"
#include "bongo/s/grid.h"
#include "bongo/s/query/cluster_cursor_manager.h"
#include "bongo/s/set_shard_version_request.h"
#include "bongo/s/write_ops/batched_command_request.h"
#include "bongo/s/write_ops/batched_command_response.h"
#include "bongo/stdx/memory.h"
#include "bongo/util/clock_source_mock.h"
#include "bongo/util/tick_source_mock.h"

namespace bongo {

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
    ShardingBongodTestFixture::setUp();
    // TODO: SERVER-26919 set the flag on the mock repl coordinator just for the window where it
    // actually needs to bypass the op observer.
    replicationCoordinator()->alwaysAllowWrites(true);

    // Initialize sharding components as a config server.
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    uassertStatusOK(initializeGlobalShardingStateForBongodForTest(ConnectionString::forLocal()));
}

std::unique_ptr<DistLockCatalog> ConfigServerTestFixture::makeDistLockCatalog(
    ShardRegistry* shardRegistry) {
    invariant(shardRegistry);
    return stdx::make_unique<DistLockCatalogImpl>(shardRegistry);
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

std::unique_ptr<ShardingCatalogManager> ConfigServerTestFixture::makeShardingCatalogManager(
    ShardingCatalogClient* catalogClient) {
    invariant(catalogClient);

    // The catalog manager requires a special executor used for operations during addShard.
    auto specialNet(stdx::make_unique<executor::NetworkInterfaceMock>());
    _mockNetworkForAddShard = specialNet.get();
    auto specialExec = makeThreadPoolTestExecutor(std::move(specialNet));
    _executorForAddShard = specialExec.get();
    _addShardNetworkTestEnv =
        stdx::make_unique<NetworkTestEnv>(specialExec.get(), _mockNetworkForAddShard);

    return stdx::make_unique<ShardingCatalogManagerImpl>(std::move(specialExec));
}

std::unique_ptr<CatalogCache> ConfigServerTestFixture::makeCatalogCache() {
    return stdx::make_unique<CatalogCache>();
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

Status ConfigServerTestFixture::insertToConfigCollection(OperationContext* txn,
                                                         const NamespaceString& ns,
                                                         const BSONObj& doc) {
    auto insert(stdx::make_unique<BatchedInsertRequest>());
    insert->addToDocuments(doc);

    BatchedCommandRequest request(insert.release());
    request.setNS(ns);

    auto config = getConfigShard();
    invariant(config);

    auto insertResponse = config->runCommand(txn,
                                             kReadPref,
                                             ns.db().toString(),
                                             request.toBSON(),
                                             Shard::kDefaultConfigCommandTimeout,
                                             Shard::RetryPolicy::kNoRetry);

    BatchedCommandResponse batchResponse;
    auto status = Shard::CommandResponse::processBatchWriteResponse(insertResponse, &batchResponse);
    return status;
}

StatusWith<BSONObj> ConfigServerTestFixture::findOneOnConfigCollection(OperationContext* txn,
                                                                       const NamespaceString& ns,
                                                                       const BSONObj& filter) {
    auto config = getConfigShard();
    invariant(config);

    auto findStatus = config->exhaustiveFindOnConfig(
        txn, kReadPref, repl::ReadConcernLevel::kMajorityReadConcern, ns, filter, BSONObj(), 1);
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

StatusWith<ShardType> ConfigServerTestFixture::getShardDoc(OperationContext* txn,
                                                           const std::string& shardId) {
    auto doc = findOneOnConfigCollection(
        txn, NamespaceString(ShardType::ConfigNS), BSON(ShardType::name(shardId)));
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

StatusWith<ChunkType> ConfigServerTestFixture::getChunkDoc(OperationContext* txn,
                                                           const BSONObj& minKey) {
    auto doc = findOneOnConfigCollection(
        txn, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::min() << minKey));
    if (!doc.isOK())
        return doc.getStatus();

    return ChunkType::fromConfigBSON(doc.getValue());
}

StatusWith<std::vector<BSONObj>> ConfigServerTestFixture::getIndexes(OperationContext* txn,
                                                                     const NamespaceString& ns) {
    auto configShard = getConfigShard();

    auto response = configShard->runCommand(txn,
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


}  // namespace bongo
