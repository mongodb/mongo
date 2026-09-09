// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
// Sets up replication services for `svcCtx`.
// TODO SERVER-122243: Make this benchmark call into some utility that is also used by
// `mongod_main`.
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

void setupCatalog(ServiceContext* svcCtx) {
    DatabaseHolder::set(svcCtx, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(svcCtx, std::make_unique<CollectionImplFactory>());
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
    // TODO SERVER-122243: Call into a helper to initialize opObservers that run on shard servers.
    svcCtx->setOpObserver(std::move(opObserverRegistry));
}

void setupCommandHooks(ServiceContext* svcCtx) {
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
void setupStorage(ServiceContext* svcCtx) {
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

class CrudBenchmarkFixture : public ServiceEntryPointBenchmarkFixture {
public:
    static constexpr auto kCollection = "test"sv;
    static constexpr auto kDatabase = "test"sv;
    static inline const std::string kDocumentData = std::string(256, 'x');
    static constexpr int kBatchSize = 100;
    static constexpr int kDeleteOneIterations = 10'000;
    // Extra documents preloaded for BM_DELETE_ONE that are never deleted, so deletes always run
    // against a collection of at least this size.
    static constexpr int kDeleteOneCollectionFloor = 10'000;

    void setUpServiceContext(ServiceContext* svcCtx) override {
        auto service = svcCtx->getService();
        service->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

        setupReplication(svcCtx, getClusterRole());
        setupCatalog(svcCtx);
        setupShardingState(svcCtx);
        setupOpObservers(svcCtx);
        setupCommandHooks(svcCtx);
        setupStorage(svcCtx);
    }

    void tearDownServiceContext(ServiceContext* svcCtx) override {
        repl::ReplicationCoordinator::get(svcCtx)->enterTerminalShutdown();
        catalog::shutDownCollectionCatalogAndGlobalStorageEngineCleanly(svcCtx, true);
    }

    ClusterRole getClusterRole() const override {
        return ClusterRole::ShardServer;
    }

protected:
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

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_FIND_ONE)
(benchmark::State& state) {
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

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_UPDATE_ONE)
(benchmark::State& state) {
    _populateTestData(getGlobalServiceContext());
    runBenchmark(state, [updateValue = int64_t{0}]() mutable {
        // clang-format off
        return BSON(
            "update" << kCollection
            << "$db" << kDatabase
            << "updates" << BSON_ARRAY(
                BSON(
                    "q" << BSON(
                        "_id" << 1
                    )
                    << "u" << BSON(
                        "$set" << BSON(
                            "data" << ++updateValue
                        )
                    )
                    << "multi" << false
                    << "upsert" << false
                )
            )
        );
        // clang-format on
    });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_FIND_AND_MODIFY)
(benchmark::State& state) {
    _populateTestData(getGlobalServiceContext());
    // Query on an extra "data" predicate to make the query express path ineligible.
    runBenchmark(state, [updateValue = int64_t{0}]() mutable {
        // clang-format off
        return BSON(
            "findAndModify" << kCollection
            << "$db" << kDatabase
            << "query" << BSON("_id" << 1 << "data" << "MongoDB")
            << "update" << BSON(
                "$set" << BSON(
                    "counter" << ++updateValue
                )
            )
            << "new" << true
            << "upsert" << false
        );
        // clang-format on
    });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_INSERT_ONE)
(benchmark::State& state) {
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

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_INSERT_MANY)
(benchmark::State& state) {
    // Prepopulate the collection so we are not benchmarking collection creation time.
    _populateTestData(getGlobalServiceContext());
    BSONArrayBuilder documents;
    for (int i = 0; i < kBatchSize; ++i) {
        documents.append(BSON("data" << kDocumentData));
    }
    // clang-format off
    BSONObj cmd = BSON(
            "insert" << kCollection
            << "$db" << kDatabase
            << "documents" << documents.arr());
    // clang-format on
    runBenchmark(state, [=] { return cmd; });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_AGG_MATCH_ONE)
(benchmark::State& state) {
    _populateTestData(getGlobalServiceContext());
    // clang-format off
    BSONObj cmd = BSON(
            "aggregate" << kCollection
            << "$db" << kDatabase
            << "pipeline" << BSON_ARRAY(
                BSON("$match" << BSON("_id" << 1))
                << BSON("$limit" << 1))
            << "cursor" << BSONObj());
    // clang-format on
    runBenchmark(state, [=] { return cmd; });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_DELETE_ONE)
(benchmark::State& state) {
    // Load one document per iteration (kDeleteOneIterations). The extra kDeleteOneCollectionFloor
    // documents are never deleted, so every iteration deletes from a collection of at least that
    // size. Ids are partitioned by thread so concurrent benchmark threads never target the same
    // document.
    const auto baseId = (int64_t{state.thread_index} + 1) << 32;
    _populateDeleteTestData(
        getGlobalServiceContext(), baseId + 1, kDeleteOneIterations + kDeleteOneCollectionFloor);
    runBenchmark(state, [idValue = baseId]() mutable {
        // clang-format off
        return BSON(
            "delete" << kCollection
            << "$db" << kDatabase
            << "deletes" << BSON_ARRAY(
                BSON(
                    "q" << BSON(
                        "_id" << ++idValue
                    )
                    << "limit" << 1
                )
            )
        );
        // clang-format on
    });
}

BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_FIND_ONE)->Threads(1)->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_UPDATE_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_FIND_AND_MODIFY)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_INSERT_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_INSERT_MANY)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_DELETE_ONE)
    ->Iterations(CrudBenchmarkFixture::kDeleteOneIterations)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_AGG_MATCH_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);

}  // namespace
}  // namespace mongo
