/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/init.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

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
            void _doTest() override {}

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

    static inline stdx::mutex _mu;
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
            PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
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
            PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
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
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         readConcern,
                                         AcquisitionPrerequisites::kRead},
            CollectionAcquisitionRequest{nss2,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
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
