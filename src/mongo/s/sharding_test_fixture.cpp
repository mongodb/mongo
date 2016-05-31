/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/s/sharding_test_fixture.h"

#include <algorithm>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_manager_impl.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/sharding_egress_metadata_hook_for_mongos.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using rpc::ShardingEgressMetadataHookForMongos;
using unittest::assertGet;

using std::string;
using std::vector;
using unittest::assertGet;

ShardingTestFixture::ShardingTestFixture() = default;

ShardingTestFixture::~ShardingTestFixture() = default;

const Seconds ShardingTestFixture::kFutureTimeout{5};

void ShardingTestFixture::setUp() {
    _service = stdx::make_unique<ServiceContextNoop>();
    _service->setFastClockSource(stdx::make_unique<ClockSourceMock>());
    _service->setPreciseClockSource(stdx::make_unique<ClockSourceMock>());
    _service->setTickSource(stdx::make_unique<TickSourceMock>());
    auto tlMock = stdx::make_unique<transport::TransportLayerMock>();
    _transportLayer = tlMock.get();
    _service->addAndStartTransportLayer(std::move(tlMock));
    _transportSession =
        stdx::make_unique<transport::Session>(HostAndPort{}, HostAndPort{}, _transportLayer);
    _client = _service->makeClient("ShardingTestFixture", _transportSession.get());
    _opCtx = _client->makeOperationContext();

    // Set up executor pool used for most operations.
    auto fixedNet = stdx::make_unique<executor::NetworkInterfaceMock>();
    fixedNet->setEgressMetadataHook(stdx::make_unique<ShardingEgressMetadataHookForMongos>());
    _mockNetwork = fixedNet.get();
    auto fixedExec = makeThreadPoolTestExecutor(std::move(fixedNet));
    _networkTestEnv = stdx::make_unique<NetworkTestEnv>(fixedExec.get(), _mockNetwork);
    _executor = fixedExec.get();

    auto netForPool = stdx::make_unique<executor::NetworkInterfaceMock>();
    netForPool->setEgressMetadataHook(stdx::make_unique<ShardingEgressMetadataHookForMongos>());
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

    auto uniqueDistLockManager = stdx::make_unique<DistLockManagerMock>();
    _distLockManager = uniqueDistLockManager.get();
    std::unique_ptr<ShardingCatalogClientImpl> catalogClient(
        stdx::make_unique<ShardingCatalogClientImpl>(std::move(uniqueDistLockManager)));
    _catalogClient = catalogClient.get();
    catalogClient->startup();

    std::unique_ptr<ShardingCatalogManagerImpl> catalogManager(
        stdx::make_unique<ShardingCatalogManagerImpl>(_catalogClient, std::move(specialExec)));
    _catalogManager = catalogManager.get();
    catalogManager->startup();

    ConnectionString configCS = ConnectionString::forReplicaSet(
        "configRS", {HostAndPort{"TestHost1"}, HostAndPort{"TestHost2"}});

    auto targeterFactory(stdx::make_unique<RemoteCommandTargeterFactoryMock>());
    auto targeterFactoryPtr = targeterFactory.get();
    _targeterFactory = targeterFactoryPtr;

    auto configTargeter(stdx::make_unique<RemoteCommandTargeterMock>());
    _configTargeter = configTargeter.get();
    _targeterFactory->addTargeterToReturn(configCS, std::move(configTargeter));

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

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::SET, std::move(setBuilder)},
        {ConnectionString::MASTER, std::move(masterBuilder)},
    };

    auto shardFactory =
        stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    auto shardRegistry(stdx::make_unique<ShardRegistry>(std::move(shardFactory), configCS));
    executorPool->startup();

    // For now initialize the global grid object. All sharding objects will be accessible from there
    // until we get rid of it.
    grid.init(std::move(catalogClient),
              std::move(catalogManager),
              stdx::make_unique<CatalogCache>(),
              std::move(shardRegistry),
              stdx::make_unique<ClusterCursorManager>(_service->getPreciseClockSource()),
              stdx::make_unique<BalancerConfiguration>(),
              std::move(executorPool),
              _mockNetwork);
}

void ShardingTestFixture::tearDown() {
    grid.getExecutorPool()->shutdownAndJoin();
    grid.catalogManager()->shutDown(_opCtx.get());
    grid.catalogClient(_opCtx.get())->shutDown(_opCtx.get());
    grid.clearForUnitTests();

    _transportSession.reset();
    _opCtx.reset();
    _client.reset();
    _service.reset();
}

void ShardingTestFixture::shutdownExecutor() {
    if (_executor) {
        _executor->shutdown();
        _executorForAddShard->shutdown();
    }
}

ShardingCatalogClient* ShardingTestFixture::catalogClient() const {
    return grid.catalogClient(_opCtx.get());
}

ShardingCatalogManager* ShardingTestFixture::catalogManager() const {
    return grid.catalogManager();
}

ShardingCatalogClientImpl* ShardingTestFixture::getCatalogClient() const {
    return _catalogClient;
}

ShardRegistry* ShardingTestFixture::shardRegistry() const {
    invariant(grid.shardRegistry());

    return grid.shardRegistry();
}

RemoteCommandTargeterFactoryMock* ShardingTestFixture::targeterFactory() const {
    invariant(_targeterFactory);

    return _targeterFactory;
}

RemoteCommandTargeterMock* ShardingTestFixture::configTargeter() const {
    invariant(_configTargeter);

    return _configTargeter;
}

executor::NetworkInterfaceMock* ShardingTestFixture::network() const {
    invariant(_mockNetwork);

    return _mockNetwork;
}

executor::TaskExecutor* ShardingTestFixture::executor() const {
    invariant(_executor);

    return _executor;
}

DistLockManagerMock* ShardingTestFixture::distLock() const {
    invariant(_distLockManager);
    return _distLockManager;
}

OperationContext* ShardingTestFixture::operationContext() const {
    invariant(_opCtx);

    return _opCtx.get();
}

void ShardingTestFixture::onCommand(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnv->onCommand(func);
}

void ShardingTestFixture::onCommandForAddShard(NetworkTestEnv::OnCommandFunction func) {
    _addShardNetworkTestEnv->onCommand(func);
}

void ShardingTestFixture::onCommandWithMetadata(
    NetworkTestEnv::OnCommandWithMetadataFunction func) {
    _networkTestEnv->onCommandWithMetadata(func);
}

void ShardingTestFixture::onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
    _networkTestEnv->onFindCommand(func);
}

void ShardingTestFixture::onFindWithMetadataCommand(
    NetworkTestEnv::OnFindCommandWithMetadataFunction func) {
    _networkTestEnv->onFindWithMetadataCommand(func);
}

void ShardingTestFixture::setupShards(const std::vector<ShardType>& shards) {
    auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });

    expectGetShards(shards);

    future.timed_get(kFutureTimeout);
}

void ShardingTestFixture::expectGetShards(const std::vector<ShardType>& shards) {
    onFindCommand([this, &shards](const RemoteCommandRequest& request) {
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), ShardType::ConfigNS);

        auto queryResult = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
        ASSERT_OK(queryResult.getStatus());

        const auto& query = queryResult.getValue();
        ASSERT_EQ(query->ns(), ShardType::ConfigNS);

        ASSERT_EQ(query->getFilter(), BSONObj());
        ASSERT_EQ(query->getSort(), BSONObj());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        vector<BSONObj> shardsToReturn;

        std::transform(shards.begin(),
                       shards.end(),
                       std::back_inserter(shardsToReturn),
                       [](const ShardType& shard) { return shard.toBSON(); });

        return shardsToReturn;
    });
}

void ShardingTestFixture::expectInserts(const NamespaceString& nss,
                                        const std::vector<BSONObj>& expected) {
    onCommand([&nss, &expected](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(nss.db(), request.dbname);

        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));

        ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().toString());

        auto inserted = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(expected.size(), inserted.size());

        auto itInserted = inserted.begin();
        auto itExpected = expected.begin();

        for (; itInserted != inserted.end(); itInserted++, itExpected++) {
            ASSERT_EQ(*itExpected, *itInserted);
        }

        BatchedCommandResponse response;
        response.setOk(true);

        return response.toBSON();
    });
}

void ShardingTestFixture::expectConfigCollectionCreate(const HostAndPort& configHost,
                                                       StringData collName,
                                                       int cappedSize,
                                                       const BSONObj& response) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCreateCmd =
            BSON("create" << collName << "capped" << true << "size" << cappedSize << "writeConcern"
                          << BSON("w"
                                  << "majority"
                                  << "wtimeout"
                                  << 15000)
                          << "maxTimeMS"
                          << 30000);
        ASSERT_EQUALS(expectedCreateCmd, request.cmdObj);

        return response;
    });
}

void ShardingTestFixture::expectConfigCollectionInsert(const HostAndPort& configHost,
                                                       StringData collName,
                                                       Date_t timestamp,
                                                       const std::string& what,
                                                       const std::string& ns,
                                                       const BSONObj& detail) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));

        ASSERT_EQ("config", actualBatchedInsert.getNS().db());
        ASSERT_EQ(collName, actualBatchedInsert.getNS().coll());

        auto inserts = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(1U, inserts.size());

        const ChangeLogType& actualChangeLog = assertGet(ChangeLogType::fromBSON(inserts.front()));

        ASSERT_EQUALS(operationContext()->getClient()->clientAddress(true),
                      actualChangeLog.getClientAddr());
        ASSERT_EQUALS(detail, actualChangeLog.getDetails());
        ASSERT_EQUALS(ns, actualChangeLog.getNS());
        ASSERT_EQUALS(network()->getHostName(), actualChangeLog.getServer());
        ASSERT_EQUALS(timestamp, actualChangeLog.getTime());
        ASSERT_EQUALS(what, actualChangeLog.getWhat());

        // Handle changeId specially because there's no way to know what OID was generated
        std::string changeId = actualChangeLog.getChangeId();
        size_t firstDash = changeId.find("-");
        size_t lastDash = changeId.rfind("-");

        const std::string serverPiece = changeId.substr(0, firstDash);
        const std::string timePiece = changeId.substr(firstDash + 1, lastDash - firstDash - 1);
        const std::string oidPiece = changeId.substr(lastDash + 1);

        ASSERT_EQUALS(grid.getNetwork()->getHostName(), serverPiece);
        ASSERT_EQUALS(timestamp.toString(), timePiece);

        OID generatedOID;
        // Just make sure this doesn't throws and assume the OID is valid
        generatedOID.init(oidPiece);

        BatchedCommandResponse response;
        response.setOk(true);

        return response.toBSON();
    });
}

void ShardingTestFixture::expectChangeLogCreate(const HostAndPort& configHost,
                                                const BSONObj& response) {
    expectConfigCollectionCreate(configHost, "changelog", 10 * 1024 * 1024, response);
}

void ShardingTestFixture::expectChangeLogInsert(const HostAndPort& configHost,
                                                Date_t timestamp,
                                                const std::string& what,
                                                const std::string& ns,
                                                const BSONObj& detail) {
    expectConfigCollectionInsert(configHost, "changelog", timestamp, what, ns, detail);
}

void ShardingTestFixture::expectUpdateCollection(const HostAndPort& expectedHost,
                                                 const CollectionType& coll) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(expectedHost, request.target);
        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);
        ASSERT_EQUALS("config", request.dbname);

        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(CollectionType::ConfigNS, actualBatchedUpdate.getNS().ns());
        auto updates = actualBatchedUpdate.getUpdates();
        ASSERT_EQUALS(1U, updates.size());
        auto update = updates.front();

        ASSERT_TRUE(update->getUpsert());
        ASSERT_FALSE(update->getMulti());
        ASSERT_EQUALS(update->getQuery(), BSON(CollectionType::fullNs(coll.getNs().toString())));
        ASSERT_EQUALS(update->getUpdateExpr(), coll.toBSON());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });
}

void ShardingTestFixture::expectSetShardVersion(const HostAndPort& expectedHost,
                                                const ShardType& expectedShard,
                                                const NamespaceString& expectedNs,
                                                const ChunkVersion& expectedChunkVersion) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(expectedHost, request.target);
        ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

        SetShardVersionRequest ssv =
            assertGet(SetShardVersionRequest::parseFromBSON(request.cmdObj));

        ASSERT(!ssv.isInit());
        ASSERT(ssv.isAuthoritative());
        ASSERT_EQ(grid.shardRegistry()->getConfigServerConnectionString().toString(),
                  ssv.getConfigServer().toString());
        ASSERT_EQ(expectedShard.getHost(), ssv.getShardConnectionString().toString());
        ASSERT_EQ(expectedNs.toString(), ssv.getNS().ns());
        ASSERT_EQ(expectedChunkVersion.toString(), ssv.getNSVersion().toString());

        return BSON("ok" << true);
    });
}

void ShardingTestFixture::expectCount(const HostAndPort& configHost,
                                      const NamespaceString& expectedNs,
                                      const BSONObj& expectedQuery,
                                      const StatusWith<long long>& response) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("count", cmdName);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQUALS(expectedNs.toString(), nss.toString());

        if (expectedQuery.isEmpty()) {
            auto queryElem = request.cmdObj["query"];
            ASSERT_TRUE(queryElem.eoo() || queryElem.Obj().isEmpty());
        } else {
            ASSERT_EQUALS(expectedQuery, request.cmdObj["query"].Obj());
        }

        if (response.isOK()) {
            return BSON("ok" << 1 << "n" << response.getValue());
        }

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        BSONObjBuilder responseBuilder;
        Command::appendCommandStatus(responseBuilder, response.getStatus());
        return responseBuilder.obj();
    });
}

void ShardingTestFixture::setRemote(const HostAndPort& remote) {
    *_transportSession = transport::Session{remote, HostAndPort{}, _transportLayer};
}

void ShardingTestFixture::checkReadConcern(const BSONObj& cmdObj,
                                           const Timestamp& expectedTS,
                                           long long expectedTerm) const {
    auto readConcernElem = cmdObj[repl::ReadConcernArgs::kReadConcernFieldName];
    ASSERT_EQ(Object, readConcernElem.type());

    auto readConcernObj = readConcernElem.Obj();
    ASSERT_EQ("majority", readConcernObj[repl::ReadConcernArgs::kLevelFieldName].str());

    auto afterElem = readConcernObj[repl::ReadConcernArgs::kAfterOpTimeFieldName];
    ASSERT_EQ(Object, afterElem.type());

    auto afterObj = afterElem.Obj();

    ASSERT_TRUE(afterObj.hasField(repl::OpTime::kTimestampFieldName));
    ASSERT_EQ(expectedTS, afterObj[repl::OpTime::kTimestampFieldName].timestamp());
    ASSERT_TRUE(afterObj.hasField(repl::OpTime::kTermFieldName));
    ASSERT_EQ(expectedTerm, afterObj[repl::OpTime::kTermFieldName].numberLong());
}

}  // namespace mongo
