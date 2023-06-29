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

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

const int kMaxPerfThreads = 16;  // max number of threads to use for lock perf

class DConcurrencyTest : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        if (state.thread_index == 0) {
            setGlobalServiceContext(ServiceContext::make());
            makeKClientsWithLockers(state.threads);
        }
    }

    void TearDown(benchmark::State& state) override {
        if (state.thread_index == 0) {
            clients.clear();
            setGlobalServiceContext({});
        }
    }

    /**
     * Returns a vector of Clients of length 'k', each of which has an OperationContext with its
     * lockState set to a LockerImpl.
     */
    void makeKClientsWithLockers(int k) {
        clients.reserve(k);

        for (int i = 0; i < k; ++i) {
            auto client = getGlobalServiceContext()->makeClient(str::stream()
                                                                << "test client for thread " << i);
            auto opCtx = client->makeOperationContext();
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
    }

protected:
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients;
};

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_StdMutex)(benchmark::State& state) {
    static auto mtx = MONGO_MAKE_LATCH();

    for (auto keepRunning : state) {
        stdx::unique_lock<Latch> lk(mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_ResourceMutexShared)(benchmark::State& state) {
    static Lock::ResourceMutex mtx("testMutex");

    for (auto keepRunning : state) {
        Lock::SharedLock lk(clients[state.thread_index].second.get(), mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_ResourceMutexExclusive)(benchmark::State& state) {
    static Lock::ResourceMutex mtx("testMutex");

    for (auto keepRunning : state) {
        Lock::ExclusiveLock lk(clients[state.thread_index].second.get(), mtx);
    }
}

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

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_StdMutex)->ThreadRange(1, kMaxPerfThreads);

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_ResourceMutexShared)->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(DConcurrencyTest, BM_ResourceMutexExclusive)->ThreadRange(1, kMaxPerfThreads);

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionIntentSharedLock)
    ->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionIntentExclusiveLock)
    ->ThreadRange(1, kMaxPerfThreads);

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionSharedLock)->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(DConcurrencyTest, BM_CollectionExclusiveLock)->ThreadRange(1, kMaxPerfThreads);

}  // namespace
}  // namespace mongo
