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

#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_impl.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client_impl.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/num_hosts_targeted_metrics.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/client/shard_factory.h"
#include "mongo/db/sharding_environment/client/shard_remote.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_initialization.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/sharding_task_executor.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mongo {

using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;

namespace {

std::shared_ptr<executor::ShardingTaskExecutor> makeShardingTestExecutor(
    std::unique_ptr<executor::NetworkInterfaceMock> net) {
    return executor::ShardingTaskExecutor::create(makeThreadPoolTestExecutor(std::move(net)));
}

}  // namespace

ShardingTestFixture::ShardingTestFixture() : ShardingTestFixture(false) {}

ShardingTestFixture::ShardingTestFixture(
    bool withMockCatalogCache,
    std::unique_ptr<ScopedGlobalServiceContextForTest> scopedServiceContext)
    : ShardingTestFixtureCommon(
          scopedServiceContext ? std::move(scopedServiceContext)
                               : std::make_unique<ScopedGlobalServiceContextForTest>(
                                     ServiceContext::make(std::make_unique<ClockSourceMock>(),
                                                          std::make_unique<ClockSourceMock>(),
                                                          std::make_unique<TickSourceMock<>>()),
                                     shouldSetupTL)) {
    const auto service = getServiceContext();

    CollatorFactoryInterface::set(service, std::make_unique<CollatorFactoryMock>());
    ShardingState::create(service);

    // Set up executor pool used for most operations.
    auto fixedNet = std::make_unique<executor::NetworkInterfaceMock>();
    fixedNet->setEgressMetadataHook(makeShardingEgressHooksList(service));
    _mockNetwork = fixedNet.get();
    _fixedExecutor = makeShardingTestExecutor(std::move(fixedNet));
    _networkTestEnv = std::make_unique<NetworkTestEnv>(_fixedExecutor.get(), _mockNetwork);

    auto netForPool = std::make_unique<executor::NetworkInterfaceMock>();
    netForPool->setEgressMetadataHook(makeShardingEgressHooksList(service));
    _mockNetworkForPool = netForPool.get();
    auto execForPool = makeShardingTestExecutor(std::move(netForPool));
    _networkTestEnvForPool =
        std::make_unique<NetworkTestEnv>(execForPool.get(), _mockNetworkForPool);
    std::vector<std::shared_ptr<executor::TaskExecutor>> executorsForPool;
    executorsForPool.emplace_back(std::move(execForPool));

    auto executorPool = std::make_unique<executor::TaskExecutorPool>();
    executorPool->addExecutors(std::move(executorsForPool), _fixedExecutor);

    NumHostsTargetedMetrics::get(service).startup();

    ConnectionString configCS = ConnectionString::forReplicaSet(
        "configRS", {HostAndPort{"TestHost1"}, HostAndPort{"TestHost2"}});

    auto targeterFactory(std::make_unique<RemoteCommandTargeterFactoryMock>());
    auto targeterFactoryPtr = targeterFactory.get();
    _targeterFactory = targeterFactoryPtr;

    ShardFactory::BuilderCallable setBuilder = [targeterFactoryPtr](
                                                   const ShardId& shardId,
                                                   const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuilderCallable masterBuilder = [targeterFactoryPtr](
                                                      const ShardId& shardId,
                                                      const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::ConnectionType::kReplicaSet, std::move(setBuilder)},
        {ConnectionString::ConnectionType::kStandalone, std::move(masterBuilder)},
    };

    auto shardFactory =
        std::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    auto shardRegistry(std::make_unique<ShardRegistry>(service, std::move(shardFactory), configCS));
    executorPool->startup();

    auto catalogCache = [&]() -> std::unique_ptr<CatalogCache> {
        if (withMockCatalogCache) {
            return std::make_unique<CatalogCacheMock>(
                service, std::make_shared<ConfigServerCatalogCacheLoaderMock>());
        } else {
            return std::make_unique<CatalogCache>(
                service, std::make_shared<ConfigServerCatalogCacheLoaderImpl>());
        }
    }();

    // For now initialize the global grid object. All sharding objects will be accessible from there
    // until we get rid of it.
    auto uniqueOpCtx = makeOperationContext();
    auto const grid = Grid::get(uniqueOpCtx.get());
    grid->init(makeShardingCatalogClient(),
               std::move(catalogCache),
               std::move(shardRegistry),
               std::make_unique<ClusterCursorManager>(service->getPreciseClockSource()),
               std::make_unique<BalancerConfiguration>(),
               std::move(executorPool),
               _mockNetwork);
}

ShardingTestFixture::~ShardingTestFixture() {
    if (auto grid = Grid::get(getServiceContext())) {
        shutdownExecutorPool();
        if (grid->shardRegistry()) {
            grid->shardRegistry()->shutdown();
        }
        grid->clearForUnitTests();
    }
}

std::shared_ptr<RemoteCommandTargeterMock> ShardingTestFixture::configTargeter() {
    return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
}

void ShardingTestFixture::shutdownExecutor() {
    if (!_fixedExecutor) {
        return;
    }
    _fixedExecutor->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(_mockNetwork)->runReadyNetworkOperations();
}

ShardingCatalogClient* ShardingTestFixture::catalogClient() const {
    return Grid::get(operationContext())->catalogClient();
}

ShardRegistry* ShardingTestFixture::shardRegistry() const {
    return Grid::get(operationContext())->shardRegistry();
}

std::shared_ptr<executor::TaskExecutor> ShardingTestFixture::executor() const {
    invariant(_fixedExecutor);

    return _fixedExecutor;
}

void ShardingTestFixture::onCommandForPoolExecutor(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnvForPool->onCommand(func);
}

void ShardingTestFixture::addRemoteShards(
    const std::vector<std::tuple<ShardId, HostAndPort>>& shardInfos) {
    std::vector<ShardType> shards;

    for (auto shard : shardInfos) {
        ShardType shardType;
        shardType.setName(std::get<0>(shard).toString());
        shardType.setHost(std::get<1>(shard).toString());
        shards.push_back(shardType);

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(ConnectionString(std::get<1>(shard)));
        targeter->setFindHostReturnValue(std::get<1>(shard));

        targeterFactory()->addTargeterToReturn(ConnectionString(std::get<1>(shard)),
                                               std::move(targeter));
    }

    setupShards(shards);
}

void ShardingTestFixture::setupShards(const std::vector<ShardType>& shards) {
    auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });

    expectGetShards(shards);

    future.default_timed_get();
}

void ShardingTestFixture::expectGetShards(const std::vector<ShardType>& shards) {
    onFindWithMetadataCommand([this, &shards](const RemoteCommandRequest& request) {
        const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, NamespaceString::kConfigsvrShardsNamespace);

        // If there is no '$db', append it.
        auto cmd = static_cast<OpMsgRequest>(request).body;
        auto query = query_request_helper::makeFromFindCommandForTests(cmd, nss);
        ASSERT_EQ(query->getNamespaceOrUUID().nss(), NamespaceString::kConfigsvrShardsNamespace);

        ASSERT_BSONOBJ_EQ(query->getFilter(), BSONObj());
        ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());
        ASSERT_FALSE(query->getLimit().has_value());

        checkReadConcern(request.cmdObj,
                         VectorClock::kInitialComponentTime.asTimestamp(),
                         repl::OpTime::kUninitializedTerm);

        std::vector<BSONObj> shardsToReturn;

        Timestamp maxTopologyTime;
        std::transform(shards.begin(),
                       shards.end(),
                       std::back_inserter(shardsToReturn),
                       [&maxTopologyTime](const ShardType& shard) {
                           maxTopologyTime = std::max(shard.getTopologyTime(), maxTopologyTime);
                           return shard.toBSON();
                       });

        BSONObjBuilder bob;
        bob.append(VectorClock::kTopologyTimeFieldName, maxTopologyTime);

        return std::make_tuple(shardsToReturn, bob.obj());
    });
}

void ShardingTestFixture::expectInserts(const NamespaceString& nss,
                                        const std::vector<BSONObj>& expected) {
    onCommand([&nss, &expected](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(nss.dbName(), request.dbname);

        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto insertOp = InsertOp::parse(opMsgRequest);
        ASSERT_EQUALS(nss, insertOp.getNamespace());

        const auto& inserted = insertOp.getDocuments();
        ASSERT_EQUALS(expected.size(), inserted.size());

        auto itInserted = inserted.begin();
        auto itExpected = expected.begin();

        for (; itInserted != inserted.end(); itInserted++, itExpected++) {
            ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
        }

        BatchedCommandResponse response;
        response.setStatus(Status::OK());

        return response.toBSON();
    });
}

void ShardingTestFixture::expectUpdateCollection(const HostAndPort& expectedHost,
                                                 const CollectionType& coll,
                                                 bool expectUpsert) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(expectedHost, request.target);
        ASSERT_EQUALS(DatabaseName::kConfig, request.dbname);
        ASSERT_BSONOBJ_EQ(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(CollectionType::ConfigNS, updateOp.getNamespace());

        const auto& updates = updateOp.getUpdates();
        ASSERT_EQUALS(1U, updates.size());

        const auto& update = updates.front();
        ASSERT_EQ(expectUpsert, update.getUpsert());
        ASSERT(!update.getMulti());
        ASSERT_BSONOBJ_EQ(BSON(CollectionType::kNssFieldName << coll.getNss().toString_forTest()),
                          update.getQ());
        const auto& updateBSON =
            update.getU().type() == write_ops::UpdateModification::Type::kReplacement
            ? update.getU().getUpdateReplacement()
            : update.getU().getUpdateModifier();
        ASSERT_BSONOBJ_EQ(coll.toBSON(), updateBSON);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });
}

void ShardingTestFixture::expectCount(const HostAndPort& configHost,
                                      const NamespaceString& expectedNs,
                                      const BSONObj& expectedQuery,
                                      const StatusWith<long long>& response) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        const std::string cmdName(request.cmdObj.firstElement().fieldName());
        ASSERT_EQUALS("count", cmdName);
        const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQUALS(expectedNs.toString_forTest(), nss.toString_forTest());

        if (expectedQuery.isEmpty()) {
            auto queryElem = request.cmdObj["query"];
            ASSERT_TRUE(queryElem.eoo() || queryElem.Obj().isEmpty());
        } else {
            ASSERT_BSONOBJ_EQ(expectedQuery, request.cmdObj["query"].Obj());
        }

        if (response.isOK()) {
            return BSON("ok" << 1 << "n" << response.getValue());
        }

        checkReadConcern(request.cmdObj,
                         VectorClock::kInitialComponentTime.asTimestamp(),
                         repl::OpTime::kUninitializedTerm);

        BSONObjBuilder responseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(responseBuilder, response.getStatus());
        return responseBuilder.obj();
    });
}

void ShardingTestFixture::expectFindSendBSONObjVector(const HostAndPort& configHost,
                                                      std::vector<BSONObj> obj) {
    onFindCommand([&, obj](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, configHost);
        ASSERT_EQ(request.dbname, DatabaseName::kConfig);
        return obj;
    });
}

void ShardingTestFixture::checkReadConcern(const BSONObj& cmdObj,
                                           const Timestamp& expectedTS,
                                           long long expectedTerm) const {
    auto readConcernElem = cmdObj[repl::ReadConcernArgs::kReadConcernFieldName];
    ASSERT_EQ(BSONType::object, readConcernElem.type());

    auto readConcernObj = readConcernElem.Obj();
    using namespace unittest::match;
    ASSERT_THAT(readConcernObj[repl::ReadConcernArgs::kLevelFieldName].str(),
                AnyOf(Eq("majority"), Eq("snapshot")));

    auto afterOpTimeElem = readConcernObj[repl::ReadConcernArgs::kAfterOpTimeFieldName];
    auto afterClusterTimeElem = readConcernObj[repl::ReadConcernArgs::kAfterClusterTimeFieldName];
    if (afterOpTimeElem.type() != BSONType::eoo) {
        ASSERT_EQ(BSONType::eoo, afterClusterTimeElem.type());
        ASSERT_EQ(BSONType::object, afterOpTimeElem.type());

        auto afterOpTimeObj = afterOpTimeElem.Obj();

        ASSERT_TRUE(afterOpTimeObj.hasField(repl::OpTime::kTimestampFieldName));
        ASSERT_EQ(expectedTS, afterOpTimeObj[repl::OpTime::kTimestampFieldName].timestamp());
        ASSERT_TRUE(afterOpTimeObj.hasField(repl::OpTime::kTermFieldName));
        ASSERT_EQ(expectedTerm, afterOpTimeObj[repl::OpTime::kTermFieldName].numberLong());
    } else {
        ASSERT_EQ(BSONType::eoo, afterOpTimeElem.type());
        ASSERT_EQ(BSONType::timestamp, afterClusterTimeElem.type());

        ASSERT_EQ(expectedTS, afterClusterTimeElem.timestamp());
    }
}

std::unique_ptr<ShardingCatalogClient> ShardingTestFixture::makeShardingCatalogClient() {
    return std::make_unique<ShardingCatalogClientImpl>(nullptr /* overrideConfigShard */);
}

}  // namespace mongo
