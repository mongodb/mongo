// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/d_concurrency.h"

#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

const int kMaxPerfThreads = 16;  // max number of threads to use for lock perf

class DConcurrencyTest : public benchmark::Fixture {
protected:
    void SetUp(benchmark::State& state) override {
        std::unique_lock ul(_mutex);
        if (state.thread_index == 0) {
            _serviceContextHolder = ServiceContext::make();
            makeKClientsWithLockers(state.threads);
            _cv.notify_all();
        } else {
            _cv.wait(ul, [&] { return clients.size() == (size_t)state.threads; });
        }
    }

    void TearDown(benchmark::State& state) override {
        std::unique_lock ul(_mutex);
        if (state.thread_index == 0) {
            clients.clear();
            _serviceContextHolder = nullptr;
            _cv.notify_all();
        } else {
            _cv.wait(ul, [&] { return !_serviceContextHolder; });
        }
    }

    void makeKClientsWithLockers(int k) {
        clients.reserve(k);

        for (int i = 0; i < k; ++i) {
            auto client = getServiceContext()->getService()->makeClient(
                str::stream() << "test client for thread " << i);
            auto opCtx = client->makeOperationContext();
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
    }

    ServiceContext* getServiceContext() const {
        return _serviceContextHolder.get();
    }

    std::mutex _mutex;
    stdx::condition_variable _cv;

    ServiceContext::UniqueServiceContext _serviceContextHolder;

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients;
};

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionIntentSharedLock)(benchmark::State& state) {
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IS);
        Lock::CollectionLock clk(clients[state.thread_index].second.get(),
                                 NamespaceString::createNamespaceString_forTest("test.coll"),
                                 MODE_IS);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionIntentExclusiveLock)(benchmark::State& state) {
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IX);
        Lock::CollectionLock clk(clients[state.thread_index].second.get(),
                                 NamespaceString::createNamespaceString_forTest("test.coll"),
                                 MODE_IX);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionSharedLock)(benchmark::State& state) {
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IS);
        Lock::CollectionLock clk(clients[state.thread_index].second.get(),
                                 NamespaceString::createNamespaceString_forTest("test.coll"),
                                 MODE_S);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionExclusiveLock)(benchmark::State& state) {
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IX);
        Lock::CollectionLock clk(clients[state.thread_index].second.get(),
                                 NamespaceString::createNamespaceString_forTest("test.coll"),
                                 MODE_X);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_SharedLock)(benchmark::State& state) {
    static ResourceMutex resMutex("BM_SharedLock");

    for (auto keepRunning : state) {
        Lock::SharedLock lk(clients[state.thread_index].second.get(), resMutex);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_ExclusiveLock)(benchmark::State& state) {
    static ResourceMutex resMutex("BM_ExclusiveLock");

    for (auto keepRunning : state) {
        Lock::ExclusiveLock lk(clients[state.thread_index].second.get(), resMutex);
    }
}

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionIntentSharedLock)
    ->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionIntentExclusiveLock)
    ->ThreadRange(1, kMaxPerfThreads);

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionSharedLock)->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionExclusiveLock)->ThreadRange(1, kMaxPerfThreads);

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_SharedLock)->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(DConcurrencyTest, BM_ExclusiveLock)->ThreadRange(1, kMaxPerfThreads);

}  // namespace
}  // namespace mongo
