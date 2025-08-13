/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_impl.h"
#include "mongo/db/local_catalog/database_holder_impl.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_bm_fixture.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"

#include <memory>

namespace mongo {
namespace {
// Sets up replication services for `svcCtx`.
// TODO SERVER-XXXXX: Make this benchmark call into some utility that is also used by `mongod_main`.
void setupReplication(ServiceContext* svcCtx, ClusterRole role) {
    auto makeStorageInterface = [](auto svcCtx) {
        repl::StorageInterface::set(svcCtx, std::make_unique<repl::StorageInterfaceImpl>());
        return repl::StorageInterface::get(svcCtx);
    };

    auto makeReplicationProcess = [](auto svcCtx, auto si) {
        auto markers = std::make_unique<repl::ReplicationConsistencyMarkersImpl>(si);
        auto recovery = std::make_unique<repl::ReplicationRecoveryImpl>(si, markers.get());
        repl::ReplicationProcess::set(svcCtx,
                                      std::make_unique<repl::ReplicationProcess>(
                                          si, std::move(markers), std::move(recovery)));
        return repl::ReplicationProcess::get(svcCtx);
    };

    auto makeTopologyCoordinatorOptions = [&]() -> repl::TopologyCoordinator::Options {
        return {Seconds{repl::maxSyncSourceLagSecs}, role};
    };

    auto makeReplicationExecutor = [](auto svcCtx) {
        ThreadPool::Options options;
        options.onCreateThread = [svcCtx](const std::string& threadName) {
            Client::initThread(threadName,
                               svcCtx->getService(ClusterRole::ShardServer),
                               Client::noSession(),
                               ClientOperationKillableByStepdown{false});
        };
        auto tp = std::make_unique<ThreadPool>(options);
        auto ni = std::make_unique<executor::NetworkInterfaceMock>();
        return executor::ThreadPoolTaskExecutor::create(std::move(tp), std::move(ni));
    };

    auto si = makeStorageInterface(svcCtx);
    auto rp = makeReplicationProcess(svcCtx, si);

    repl::ReplicationCoordinator::set(
        svcCtx,
        std::make_unique<repl::ReplicationCoordinatorImpl>(
            svcCtx,
            getGlobalReplSettings(),
            std::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(svcCtx, si, rp),
            makeReplicationExecutor(svcCtx),
            std::make_unique<repl::TopologyCoordinator>(makeTopologyCoordinatorOptions()),
            rp,
            si,
            SecureRandom().nextInt64()));

    MongoDSessionCatalog::set(
        svcCtx,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    IndexBuildsCoordinator::set(svcCtx, std::make_unique<IndexBuildsCoordinatorMongod>());
}

void setupCatalog(ServiceContext* svcCtx) {
    DatabaseHolder::set(svcCtx, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(svcCtx, std::make_unique<CollectionImpl::FactoryImpl>());
}

void setupShardingState(ServiceContext* svcCtx) {
    ShardingState::create(svcCtx);
    CollectionShardingStateFactory::set(
        svcCtx, std::make_unique<CollectionShardingStateFactoryShard>(svcCtx));
    DatabaseShardingStateFactory::set(svcCtx,
                                      std::make_unique<DatabaseShardingStateFactoryShard>());
}

void setupOpObservers(ServiceContext* svcCtx) {
    auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
    // TODO SERVER-XXXXX: Call into a helper to initialize opObservers that run on shard servers.
    svcCtx->setOpObserver(std::move(opObserverRegistry));
}

// Sets up storage engine with a clean slate.
void setupStorage(ServiceContext* svcCtx, ClusterRole role) {
    constexpr auto kDBPrefix = "/tmp/bench";
    storageGlobalParams.dbpath = fmt::format("{}-{}", kDBPrefix, ProcessId::getCurrent().asInt64());
    boost::filesystem::remove_all(storageGlobalParams.dbpath);
    boost::filesystem::create_directory(storageGlobalParams.dbpath);

    auto service = svcCtx->getService(role);
    auto strand = ClientStrand::make(service->makeClient("storage-initializer", nullptr));
    strand->run([&] {
        BSONObjBuilder bob;
        catalog::startUpStorageEngineAndCollectionCatalog(
            svcCtx, &cc(), StorageEngineInitFlags{}, &bob);
        StorageControl::startStorageControls(svcCtx);
    });
}

class FindOneBenchmarkFixture : public ServiceEntryPointBenchmarkFixture {
public:
    void setUpServiceContext(ServiceContext* svcCtx) override {
        auto service = svcCtx->getService(getClusterRole());
        service->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

        setupReplication(svcCtx, getClusterRole());
        setupCatalog(svcCtx);
        setupShardingState(svcCtx);
        setupOpObservers(svcCtx);
        setupStorage(svcCtx, getClusterRole());

        _populateTestData(svcCtx);
    }

    void tearDownServiceContext(ServiceContext* svcCtx) override {
        repl::ReplicationCoordinator::get(svcCtx)->enterTerminalShutdown();
        catalog::shutDownCollectionCatalogAndGlobalStorageEngineCleanly(svcCtx, true);
    }

    ClusterRole getClusterRole() const override {
        return ClusterRole::ShardServer;
    }

    static auto makeFindOneById() {
        return BSON("find" << kCollection << "$db" << kDatabase << "filter" << BSON("_id" << 1)
                           << "batchSize" << 1 << "limit" << 1 << "singleBatch" << true);
    }

private:
    void _populateTestData(ServiceContext* svcCtx) {
        auto service = svcCtx->getService(getClusterRole());
        auto strand = ClientStrand::make(service->makeClient("db-initializer", nullptr));
        strand->run([&] {
            OpMsgRequest request;
            request.body = BSON("insert" << kCollection << "$db" << kDatabase << "documents"
                                         << BSON_ARRAY(BSON("_id" << 1 << "data"
                                                                  << "MongoDB")));
            auto msg = request.serialize();
            doRequest(service->getServiceEntryPoint(), strand->getClientPointer(), msg);
        });
    }

    static constexpr auto kCollection = "test"_sd;
    static constexpr auto kDatabase = "test"_sd;
};

BENCHMARK_DEFINE_F(FindOneBenchmarkFixture, BM_FIND_ONE)
(benchmark::State& state) {
    runBenchmark(state, makeFindOneById());
}

BENCHMARK_REGISTER_F(FindOneBenchmarkFixture, BM_FIND_ONE)->ThreadRange(1, kCommandBMMaxThreads);

}  // namespace
}  // namespace mongo
