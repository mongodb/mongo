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

#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"

#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
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
        stdx::unique_lock ul(_mutex);
        if (state.thread_index == 0) {
            _serviceContextHolder = ServiceContext::make();
            makeKClientsWithLockers(state.threads);
            _cv.notify_all();
        } else {
            _cv.wait(ul, [&] { return clients.size() == (size_t)state.threads; });
        }
    }

    void TearDown(benchmark::State& state) override {
        stdx::unique_lock ul(_mutex);
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

    stdx::mutex _mutex;
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
