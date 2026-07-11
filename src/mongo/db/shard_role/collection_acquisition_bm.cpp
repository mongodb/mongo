// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#include <mutex>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest;

namespace mongo::repl {
namespace {


MONGO_INITIALIZER_GENERAL(DisableLogging, (), ())
(InitializerContext*) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

constexpr bool superVerbose = false;

/**
 * Multithreaded benchmarks are tricky.
 * Several instances will spawn. They synchronize on the state.KeepRunning()
 * evaluation. The first eval blocks until all instances (threads) reach it.
 * Then when it is about to return false, it blocks to wait for all threads
 * to be finished.
 *
 * Global init is accomplished by managing a singleton shared by all
 * instances.
 */
class CollectionAcquisitionBenchmark {
public:
    explicit CollectionAcquisitionBenchmark(benchmark::State& state) : _state{state} {
        if constexpr (superVerbose)
            std::cout << fmt::format("CollectionAcquisitionBenchmark ctor: thread=[{}/{}]\n",
                                     _state.thread_index,
                                     _state.threads);
    }
    ~CollectionAcquisitionBenchmark() {
        if constexpr (superVerbose)
            std::cout << fmt::format("CollectionAcquisitionBenchmark dtor: thread=[{}/{}]\n",
                                     _state.thread_index,
                                     _state.threads);
    }

    template <typename F>
    void operator()(F threadBody) {
        _fixture = _ensureTestEnv();
        threadBody(_state, *this);
    }

    OperationContext* getOperationContext() {
        if constexpr (superVerbose)
            std::cout << fmt::format(
                "getOperationContext (thread=[{}/{}])\n", _state.thread_index, _state.threads);
        return _uniqueOpCtx.get();
    }

private:
    /**
     * The care and feeding of the Test fixture.
     * It must run in a separate thread from the benchmark tasks,
     * because whichever thread creates it, creates a thread local Client,
     * and would be inappropriate for use as a benchmark task thread.
     */
    class TestEnv {
    public:
        TestEnv() {
            if constexpr (superVerbose)
                std::cout << fmt::format("Creating TestEnv @{}\n", (void*)this);
            _thread = unittest::JoinThread([this] {
                auto uniqueTest = std::make_unique<Test>();
                _test = uniqueTest.get();
                _test->setUp();
                _running.promise.emplaceValue();
                _stopRequest.future.get();
                _test->tearDown();
            });
            _running.future.get();
        }

        ~TestEnv() {
            if constexpr (superVerbose)
                std::cout << "Destroying TestEnv\n";
            _stopRequest.promise.emplaceValue();
        }

        Service* getService() const {
            return _test->getService();
        }

        ServiceContext::UniqueOperationContext makeOperationContext() {
            return _test->makeOperationContext();
        }

    private:
        /** A `unittest::Test` fixture being overloaded as a benchmark harness. */
        class Test : public ServiceContextMongoDTest {
        public:
            void TestBody() override {}

            void setUp() override {
                ServiceContextMongoDTest::setUp();

                auto sc = getServiceContext();
                ReplicationCoordinator::set(sc, std::make_unique<ReplicationCoordinatorMock>(sc));
            }

            using ServiceContextMongoDTest::tearDown;  // Widen visibility from protected to public
        };

        PromiseAndFuture<void> _running;
        PromiseAndFuture<void> _stopRequest;
        unittest::JoinThread _thread;
        Test* _test;
    };

    std::shared_ptr<TestEnv> _ensureTestEnv() {
        std::unique_lock lk{_mu};
        auto sp = _fixtureWeak.lock();
        if (!sp) {
            if constexpr (superVerbose)
                std::cout << "Need to create fixture\n";
            _fixtureWeak = sp = std::make_shared<TestEnv>();
            if constexpr (superVerbose)
                std::cout << "Created fixture\n";
        } else {
            if constexpr (superVerbose)
                std::cout << "Sharing fixture\n";
        }
        if constexpr (superVerbose)
            std::cout << fmt::format("Got fixture @{}\n", (void*)sp.get());

        _threadClient.emplace(sp->getService());
        _uniqueOpCtx = sp->makeOperationContext();
        return sp;
    }

    static inline std::mutex _mu;
    static inline std::weak_ptr<TestEnv> _fixtureWeak;

    benchmark::State& _state;
    std::shared_ptr<TestEnv> _fixture;
    boost::optional<ThreadClient> _threadClient;
    ServiceContext::UniqueOperationContext _uniqueOpCtx;
};

void BM_acquireCollectionLockFreeFunc(benchmark::State& state,
                                      CollectionAcquisitionBenchmark& fixture) {
    auto opCtx = fixture.getOperationContext();

    const auto nss = NamespaceString::createNamespaceString_forTest("test.test");
    const auto readConcern = repl::ReadConcernArgs::kLocal;
    for (auto _ : state) {
        // Building the request is part of the work required to acquire a collection, so we include
        // this in the benchmark.
        CollectionAcquisitionRequest request{
            nss,
            PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
            readConcern,
            AcquisitionPrerequisites::kRead};
        auto acquisition = acquireCollectionMaybeLockFree(opCtx, request);
        benchmark::DoNotOptimize(acquisition);
    }
}
void BM_acquireCollectionLockFree(benchmark::State& state) {
    CollectionAcquisitionBenchmark{state}(BM_acquireCollectionLockFreeFunc);
}

void BM_acquireCollectionFunc(benchmark::State& state, CollectionAcquisitionBenchmark& fixture) {
    auto opCtx = fixture.getOperationContext();

    const auto nss = NamespaceString::createNamespaceString_forTest("test.test");
    const auto readConcern = repl::ReadConcernArgs::kLocal;
    for (auto _ : state) {
        // Building the request is part of the work required to acquire a collection, so we include
        // this in the benchmark.
        CollectionAcquisitionRequest request{
            nss,
            PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
            readConcern,
            AcquisitionPrerequisites::kRead};
        auto acquisition = acquireCollection(opCtx, request, MODE_IS);
        benchmark::DoNotOptimize(acquisition);
    }
}
void BM_acquireCollection(benchmark::State& state) {
    CollectionAcquisitionBenchmark{state}(BM_acquireCollectionFunc);
}

void BM_acquireMultiCollectionFunc(benchmark::State& state,
                                   CollectionAcquisitionBenchmark& fixture) {
    auto opCtx = fixture.getOperationContext();

    const auto nss1 = NamespaceString::createNamespaceString_forTest("test.test1");
    const auto nss2 = NamespaceString::createNamespaceString_forTest("test.test2");
    const auto readConcern = repl::ReadConcernArgs::kLocal;
    for (auto _ : state) {
        // Building the request is part of the work required to acquire a collection, so we include
        // this in the benchmark.
        auto requests = {
            CollectionAcquisitionRequest{nss1,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         readConcern,
                                         AcquisitionPrerequisites::kRead},
            CollectionAcquisitionRequest{nss2,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         readConcern,
                                         AcquisitionPrerequisites::kRead}};
        auto acquisitions = acquireCollections(opCtx, requests, MODE_IX);
        benchmark::DoNotOptimize(acquisitions);
    }
}
void BM_acquireMultiCollection(benchmark::State& state) {
    CollectionAcquisitionBenchmark{state}(BM_acquireMultiCollectionFunc);
}

BENCHMARK(BM_acquireCollectionLockFree)->ThreadRange(1, 16);
BENCHMARK(BM_acquireCollection)->ThreadRange(1, 16);
BENCHMARK(BM_acquireMultiCollection)->ThreadRange(1, 16);
}  // namespace
}  // namespace mongo::repl
