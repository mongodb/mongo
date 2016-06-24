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
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/replset/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_manager_impl.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_local.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/sharding_egress_metadata_hook_for_mongod.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using rpc::ShardingEgressMetadataHookForMongod;
using unittest::assertGet;

using std::string;
using std::vector;
using unittest::assertGet;

namespace {
ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly);
}

ConfigServerTestFixture::ConfigServerTestFixture() = default;

ConfigServerTestFixture::~ConfigServerTestFixture() = default;

const Seconds ConfigServerTestFixture::kFutureTimeout{5};

void ConfigServerTestFixture::setUp() {
    ServiceContextMongoDTest::setUp();

    auto serviceContext = getGlobalServiceContext();
    _messagePort = stdx::make_unique<MessagingPortMock>();
    Client::initThreadIfNotAlready("ConfigServerTestFixture");
    _opCtx = cc().makeOperationContext();

    repl::ReplSettings replSettings;
    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorMock>(replSettings);
    repl::ReplicaSetConfig config;
    config.initialize(BSON("_id"
                           << "mySet"
                           << "protocolVersion"
                           << 1
                           << "version"
                           << 3
                           << "members"
                           << BSON_ARRAY(BSON("host"
                                              << "node2:12345"
                                              << "_id"
                                              << 1))));
    replCoord->setGetConfigReturnValue(config);
    repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;

    // Set up executor pool used for most operations.
    auto fixedNet = stdx::make_unique<executor::NetworkInterfaceMock>();
    fixedNet->setEgressMetadataHook(stdx::make_unique<ShardingEgressMetadataHookForMongod>());
    _mockNetwork = fixedNet.get();
    auto fixedExec = makeThreadPoolTestExecutor(std::move(fixedNet));
    _networkTestEnv = stdx::make_unique<NetworkTestEnv>(fixedExec.get(), _mockNetwork);
    _executor = fixedExec.get();

    auto netForPool = stdx::make_unique<executor::NetworkInterfaceMock>();
    netForPool->setEgressMetadataHook(stdx::make_unique<ShardingEgressMetadataHookForMongod>());
    auto execForPool = makeThreadPoolTestExecutor(std::move(netForPool));
    std::vector<std::unique_ptr<executor::TaskExecutor>> executorsForPool;
    executorsForPool.emplace_back(std::move(execForPool));

    auto executorPool = stdx::make_unique<executor::TaskExecutorPool>();
    executorPool->addExecutors(std::move(executorsForPool), std::move(fixedExec));

    // Set up executor used for a few special operations during addShard.
    auto specialNet(stdx::make_unique<executor::NetworkInterfaceMock>());
    auto specialMockNet = specialNet.get();
    auto specialExec = makeThreadPoolTestExecutor(std::move(specialNet));
    _addShardNetworkTestEnv = stdx::make_unique<NetworkTestEnv>(specialExec.get(), specialMockNet);
    _executorForAddShard = specialExec.get();

    auto targeterFactory(stdx::make_unique<RemoteCommandTargeterFactoryMock>());
    auto targeterFactoryPtr = targeterFactory.get();
    _targeterFactory = targeterFactoryPtr;

    ShardFactory::BuilderCallable setBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuilderCallable masterBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuilderCallable localBuilder = [](const ShardId& shardId,
                                                    const ConnectionString& connStr) {
        return stdx::make_unique<ShardLocal>(shardId);
    };

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::SET, std::move(setBuilder)},
        {ConnectionString::MASTER, std::move(masterBuilder)},
        {ConnectionString::LOCAL, std::move(localBuilder)},
    };

    auto shardFactory =
        stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    auto shardRegistry(
        stdx::make_unique<ShardRegistry>(std::move(shardFactory), ConnectionString::forLocal()));
    executorPool->startup();

    auto distLockCatalog = stdx::make_unique<DistLockCatalogImpl>(shardRegistry.get());

    auto uniqueDistLockManager =
        stdx::make_unique<ReplSetDistLockManager>(serviceContext,
                                                  "distLockProcessId",
                                                  std::move(distLockCatalog),
                                                  ReplSetDistLockManager::kDistLockPingInterval,
                                                  ReplSetDistLockManager::kDistLockExpirationTime);
    _distLockManager = uniqueDistLockManager.get();
    std::unique_ptr<ShardingCatalogClientImpl> catalogClient(
        stdx::make_unique<ShardingCatalogClientImpl>(std::move(uniqueDistLockManager)));
    _catalogClient = catalogClient.get();

    std::unique_ptr<ShardingCatalogManagerImpl> catalogManager(
        stdx::make_unique<ShardingCatalogManagerImpl>(_catalogClient, std::move(specialExec)));
    _catalogManager = catalogManager.get();

    // For now initialize the global grid object. All sharding objects will be accessible from there
    // until we get rid of it.
    grid.init(std::move(catalogClient),
              std::move(catalogManager),
              stdx::make_unique<CatalogCache>(),
              std::move(shardRegistry),
              stdx::make_unique<ClusterCursorManager>(serviceContext->getPreciseClockSource()),
              stdx::make_unique<BalancerConfiguration>(),
              std::move(executorPool),
              _mockNetwork);

    _catalogClient->startup();
    _catalogManager->startup();
}

void ConfigServerTestFixture::tearDown() {
    grid.getExecutorPool()->shutdownAndJoin();
    grid.catalogManager()->shutDown(_opCtx.get());
    grid.catalogClient(_opCtx.get())->shutDown(_opCtx.get());
    grid.clearForUnitTests();

    _opCtx.reset();
    _client.reset();

    ServiceContextMongoDTest::tearDown();
}

void ConfigServerTestFixture::shutdownExecutor() {
    if (_executor) {
        _executor->shutdown();
        _executorForAddShard->shutdown();
    }
}

ShardingCatalogClient* ConfigServerTestFixture::catalogClient() const {
    return grid.catalogClient(_opCtx.get());
}

ShardingCatalogManager* ConfigServerTestFixture::catalogManager() const {
    return grid.catalogManager();
}

ShardingCatalogClientImpl* ConfigServerTestFixture::getCatalogClient() const {
    return _catalogClient;
}

ShardRegistry* ConfigServerTestFixture::shardRegistry() const {
    invariant(grid.shardRegistry());

    return grid.shardRegistry();
}

RemoteCommandTargeterFactoryMock* ConfigServerTestFixture::targeterFactory() const {
    invariant(_targeterFactory);

    return _targeterFactory;
}

std::shared_ptr<Shard> ConfigServerTestFixture::getConfigShard() const {
    auto shardRegistry = grid.shardRegistry();
    invariant(shardRegistry);

    return shardRegistry->getConfigShard();
}

executor::NetworkInterfaceMock* ConfigServerTestFixture::network() const {
    invariant(_mockNetwork);

    return _mockNetwork;
}

executor::TaskExecutor* ConfigServerTestFixture::executor() const {
    invariant(_executor);

    return _executor;
}

MessagingPortMock* ConfigServerTestFixture::getMessagingPort() const {
    return _messagePort.get();
}

ReplSetDistLockManager* ConfigServerTestFixture::distLock() const {
    invariant(_distLockManager);
    return _distLockManager;
}

OperationContext* ConfigServerTestFixture::operationContext() const {
    invariant(_opCtx);

    return _opCtx.get();
}

void ConfigServerTestFixture::onCommand(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnv->onCommand(func);
}

void ConfigServerTestFixture::onCommandForAddShard(NetworkTestEnv::OnCommandFunction func) {
    _addShardNetworkTestEnv->onCommand(func);
}

void ConfigServerTestFixture::onCommandWithMetadata(
    NetworkTestEnv::OnCommandWithMetadataFunction func) {
    _networkTestEnv->onCommandWithMetadata(func);
}

void ConfigServerTestFixture::onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
    _networkTestEnv->onFindCommand(func);
}

void ConfigServerTestFixture::onFindWithMetadataCommand(
    NetworkTestEnv::OnFindCommandWithMetadataFunction func) {
    _networkTestEnv->onFindWithMetadataCommand(func);
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

    auto insertResponse = config->runCommand(
        txn, kReadPref, ns.db().toString(), request.toBSON(), Shard::RetryPolicy::kNoRetry);

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

StatusWith<std::vector<BSONObj>> ConfigServerTestFixture::getIndexes(OperationContext* txn,
                                                                     const NamespaceString& ns) {
    auto configShard = getConfigShard();

    auto response = configShard->runCommand(txn,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            ns.db().toString(),
                                            BSON("listIndexes" << ns.coll().toString()),
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


}  // namespace mongo
