// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/query/query_settings/query_settings_command_hooks.h"
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
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/collection_impl.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/shard_role/shard_catalog/database_holder_impl.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace mongo {

// Sets up replication services for `svcCtx`.
// TODO SERVER-122243: Make this benchmark call into some utility that is also used by
// `mongod_main`.
inline void crudBmSetupReplication(ServiceContext* svcCtx, ClusterRole role) {
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
        auto tp = ThreadPool::make({
            .onCreateThread =
                [svcCtx](const std::string& threadName) {
                    Client::initThread(threadName,
                                       svcCtx->getService(),
                                       Client::noSession(),
                                       ClientOperationKillableByStepdown{false});
                },
        });
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

inline void crudBmSetupCatalog(ServiceContext* svcCtx) {
    DatabaseHolder::set(svcCtx, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(svcCtx, std::make_unique<CollectionImplFactory>());
}

inline void crudBmSetupShardingState(ServiceContext* svcCtx) {
    ShardingState::create(svcCtx);
    CollectionShardingStateFactory::set(
        svcCtx, std::make_unique<CollectionShardingStateFactoryShard>(svcCtx));
    DatabaseShardingStateFactory::set(svcCtx,
                                      std::make_unique<DatabaseShardingStateFactoryShard>());
}

inline void crudBmSetupOpObservers(ServiceContext* svcCtx) {
    auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
    // TODO SERVER-122243: Call into a helper to initialize opObservers that run on shard servers.
    svcCtx->setOpObserver(std::move(opObserverRegistry));
}

inline void crudBmSetupCommandHooks(ServiceContext* svcCtx) {
    class BenchmarkCommandInvocationHooks final : public CommandInvocationHooks {
    public:
        void onBeforeRun(OperationContext* opCtx, CommandInvocation* invocation) override {
            _querySettingsHook.onBeforeRun(opCtx, invocation);
        }

        void onAfterRun(OperationContext* opCtx,
                        CommandInvocation* invocation,
                        rpc::ReplyBuilderInterface* response) override {
            _querySettingsHook.onAfterRun(opCtx, invocation, response);
        }

    private:
        query_settings::QuerySettingsCommandHooks _querySettingsHook{};
    };

    CommandInvocationHooks::set(svcCtx, std::make_unique<BenchmarkCommandInvocationHooks>());
}

// Sets up storage engine with a clean slate.
inline void crudBmSetupStorage(ServiceContext* svcCtx) {
    constexpr auto kDBPrefix = "/tmp/bench";
    storageGlobalParams.dbpath = fmt::format("{}-{}", kDBPrefix, ProcessId::getCurrent().asInt64());
    boost::filesystem::remove_all(storageGlobalParams.dbpath);
    boost::filesystem::create_directory(storageGlobalParams.dbpath);

    auto service = svcCtx->getService();
    auto strand = ClientStrand::make(service->makeClient("storage-initializer", nullptr));
    strand->run([&] {
        BSONObjBuilder bob;
        catalog::startUpStorageEngineAndCollectionCatalog(
            svcCtx, &cc(), StorageEngineInitFlags{}, &bob);
        StorageControl::startStorageControls(svcCtx);
    });
}

/**
 * Shared by crud_bm (gating; plain CRUD ops) and crud_tracing_bm (not gating; measures tracing's
 * cost on top of these same operations), so that a tracing-only addition does not grow crud_bm.
 */
class CrudBenchmarkFixture : public ServiceEntryPointBenchmarkFixture {
public:
    static constexpr std::string_view kCollection = "test";
    static constexpr std::string_view kDatabase = "test";
    static inline const std::string kDocumentData = std::string(256, 'x');
    static constexpr int kBatchSize = 100;
    // A second collection for the heavier query shapes, so they don't share documents with the
    // point-operation benchmarks.
    static constexpr std::string_view kScanCollection = "scan";
    static constexpr int kScanDocuments = 1'000;
    static constexpr int kDeleteOneIterations = 10'000;
    // Extra documents preloaded for BM_DELETE_ONE that are never deleted, so deletes always run
    // against a collection of at least this size.
    static constexpr int kDeleteOneCollectionFloor = 10'000;

    void setUpServiceContext(ServiceContext* svcCtx) override {
        auto service = svcCtx->getService();
        service->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

        crudBmSetupReplication(svcCtx, getClusterRole());
        crudBmSetupCatalog(svcCtx);
        crudBmSetupShardingState(svcCtx);
        crudBmSetupOpObservers(svcCtx);
        crudBmSetupCommandHooks(svcCtx);
        crudBmSetupStorage(svcCtx);
    }

    void tearDownServiceContext(ServiceContext* svcCtx) override {
        repl::ReplicationCoordinator::get(svcCtx)->enterTerminalShutdown();
        catalog::shutDownCollectionCatalogAndGlobalStorageEngineCleanly(svcCtx, true);
    }

    ClusterRole getClusterRole() const override {
        return ClusterRole::ShardServer;
    }

protected:
    void _runFindOne(benchmark::State& state) {
        _populateTestData(getGlobalServiceContext());
        // clang-format off
        BSONObj cmd = BSON(
                "find" << kCollection
                << "$db" << kDatabase
                << "filter" << BSON("_id" << 1)
                << "limit" << 1
                << "singleBatch" << true);
        // clang-format on
        runBenchmark(state, [=] { return cmd; });
    }

    void _runInsertOne(benchmark::State& state) {
        // Prepopulate the collection so we are not benchmarking collection creation time.
        _populateTestData(getGlobalServiceContext());
        // clang-format off
        BSONObj cmd = BSON(
                "insert" << kCollection
                << "$db" << kDatabase
                << "documents" << BSON_ARRAY(BSON("data" << kDocumentData)));
        // clang-format on
        runBenchmark(state, [=] { return cmd; });
    }

    void _runFindIndexedRange(benchmark::State& state) {
        _populateScanData(getGlobalServiceContext());
        // clang-format off
        BSONObj cmd = BSON(
                "find" << kScanCollection
                << "$db" << kDatabase
                << "filter" << BSON("a" << BSON("$gte" << 100 << "$lt" << 200))
                << "batchSize" << kScanDocuments
                << "singleBatch" << true);
        // clang-format on
        runBenchmark(state, [=] { return cmd; });
    }

    void _runFindCollscan(benchmark::State& state) {
        _populateScanData(getGlobalServiceContext());
        // clang-format off
        BSONObj cmd = BSON(
                "find" << kScanCollection
                << "$db" << kDatabase
                << "filter" << BSON("g" << 3)
                << "batchSize" << kScanDocuments
                << "singleBatch" << true);
        // clang-format on
        runBenchmark(state, [=] { return cmd; });
    }

    void _runAggGroup(benchmark::State& state) {
        _populateScanData(getGlobalServiceContext());
        // clang-format off
        BSONObj cmd = BSON(
                "aggregate" << kScanCollection
                << "$db" << kDatabase
                << "pipeline" << BSON_ARRAY(
                    BSON("$group" << BSON("_id" << "$g" << "n" << BSON("$sum" << 1))))
                << "cursor" << BSONObj());
        // clang-format on
        runBenchmark(state, [=] { return cmd; });
    }

    /**
     * Loads kScanDocuments documents into kScanCollection, with `a` (indexed) equal to the _id and
     * `g` (unindexed) cycling through 10 values. Idempotent, so every benchmark thread may call it.
     */
    void _populateScanData(ServiceContext* svcCtx) {
        auto service = svcCtx->getService();
        auto strand = ClientStrand::make(service->makeClient("db-initializer", nullptr));
        strand->run([&] {
            auto run = [&](BSONObj body) {
                OpMsgRequest request;
                request.body = std::move(body);
                auto msg = request.serialize();
                doRequest(service->getServiceEntryPoint(), strand->getClientPointer(), msg);
            };
            run(BSON("createIndexes" << kScanCollection << "$db" << kDatabase << "indexes"
                                     << BSON_ARRAY(BSON("key" << BSON("a" << 1) << "name"
                                                              << "a_1"))));
            for (int batchStart = 0; batchStart < kScanDocuments; batchStart += kBatchSize) {
                BSONArrayBuilder documents;
                for (int i = batchStart; i < batchStart + kBatchSize; ++i) {
                    documents.append(
                        BSON("_id" << i << "a" << i << "g" << i % 10 << "data" << kDocumentData));
                }
                run(BSON("insert" << kScanCollection << "$db" << kDatabase << "ordered" << false
                                  << "documents" << documents.arr()));
            }
        });
    }

    void _populateTestData(ServiceContext* svcCtx) {
        auto service = svcCtx->getService();
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

    // Bulk loads `count` documents with _ids [baseId, baseId + count) for BM_DELETE_ONE.
    void _populateDeleteTestData(ServiceContext* svcCtx, int64_t baseId, int64_t count) {
        auto service = svcCtx->getService();
        auto strand = ClientStrand::make(service->makeClient("db-initializer", nullptr));
        strand->run([&] {
            for (int64_t batchStart = 0; batchStart < count; batchStart += kBatchSize) {
                const auto batchEnd = std::min<int64_t>(batchStart + kBatchSize, count);
                BSONArrayBuilder documents;
                for (auto i = batchStart; i < batchEnd; ++i) {
                    documents.append(BSON("_id" << baseId + i << "data" << kDocumentData));
                }
                OpMsgRequest request;
                request.body = BSON("insert" << kCollection << "$db" << kDatabase << "ordered"
                                             << false << "documents" << documents.arr());
                auto msg = request.serialize();
                doRequest(service->getServiceEntryPoint(), strand->getClientPointer(), msg);
            }
        });
    }
};

}  // namespace mongo
