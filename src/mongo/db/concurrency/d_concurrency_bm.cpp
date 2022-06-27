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

#include "mongo/platform/basic.h"
#include <benchmark/benchmark.h>

#include "mongo/base/init.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/platform/mutex.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

const int kMaxPerfThreads = 16;  // max number of threads to use for lock perf

MONGO_INITIALIZER_GENERAL(DConcurrencyTestServiceContext, ("DConcurrencyTestClientObserver"), ())
(InitializerContext* context) {
    setGlobalServiceContext(ServiceContext::make());
}

class LockerImplClientObserver : public ServiceContext::ClientObserver {
public:
    LockerImplClientObserver() = default;
    ~LockerImplClientObserver() = default;

    void onCreateClient(Client* client) final {}

    void onDestroyClient(Client* client) final {}

    void onCreateOperationContext(OperationContext* opCtx) override {
        opCtx->setLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    }

    void onDestroyOperationContext(OperationContext* opCtx) final {}
};

const ServiceContext::ConstructorActionRegisterer clientObserverRegisterer{
    "DConcurrencyTestClientObserver",
    [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<LockerImplClientObserver>());
    },
    [](ServiceContext* serviceContext) {}};

class DConcurrencyTest : public benchmark::Fixture {
public:
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
            locker[i] = std::make_unique<LockerImpl>(opCtx->getServiceContext());
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
    }

protected:
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients;
    std::array<std::unique_ptr<LockerImpl>, kMaxPerfThreads> locker;
};

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_StdMutex)(benchmark::State& state) {
    if (state.thread_index == 0) {
        makeKClientsWithLockers(state.threads);
    }

    static auto mtx = MONGO_MAKE_LATCH();

    for (auto keepRunning : state) {
        stdx::unique_lock<Latch> lk(mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_ResourceMutexShared)(benchmark::State& state) {
    if (state.thread_index == 0) {
        makeKClientsWithLockers(state.threads);
    }

    static Lock::ResourceMutex mtx("testMutex");

    for (auto keepRunning : state) {
        Lock::SharedLock lk(locker[state.thread_index].get(), mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_ResourceMutexExclusive)(benchmark::State& state) {
    if (state.thread_index == 0) {
        makeKClientsWithLockers(state.threads);
    }

    static Lock::ResourceMutex mtx("testMutex");

    for (auto keepRunning : state) {
        Lock::ExclusiveLock lk(locker[state.thread_index].get(), mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionIntentSharedLock)(benchmark::State& state) {
    if (state.thread_index == 0) {
        makeKClientsWithLockers(state.threads);
    }

    DatabaseName dbName(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IS);
        Lock::CollectionLock clk(
            clients[state.thread_index].second.get(), NamespaceString("test.coll"), MODE_IS);
    }

    if (state.thread_index == 0) {
        clients.clear();
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionIntentExclusiveLock)(benchmark::State& state) {
    if (state.thread_index == 0) {
        makeKClientsWithLockers(state.threads);
    }

    DatabaseName dbName(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IX);
        Lock::CollectionLock clk(
            clients[state.thread_index].second.get(), NamespaceString("test.coll"), MODE_IX);
    }

    if (state.thread_index == 0) {
        clients.clear();
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionSharedLock)(benchmark::State& state) {
    if (state.thread_index == 0) {
        makeKClientsWithLockers(state.threads);
    }

    DatabaseName dbName(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IS);
        Lock::CollectionLock clk(
            clients[state.thread_index].second.get(), NamespaceString("test.coll"), MODE_S);
    }

    if (state.thread_index == 0) {
        clients.clear();
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionExclusiveLock)(benchmark::State& state) {
    if (state.thread_index == 0) {
        makeKClientsWithLockers(state.threads);
    }

    DatabaseName dbName(boost::none, "test");
    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), dbName, MODE_IX);
        Lock::CollectionLock clk(
            clients[state.thread_index].second.get(), NamespaceString("test.coll"), MODE_X);
    }

    if (state.thread_index == 0) {
        clients.clear();
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
