// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/lock_manager.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
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

class LockManagerTest : public benchmark::Fixture {
protected:
    void SetUp(benchmark::State& state) override {
        std::unique_lock ul(_mutex);
        if (state.thread_index == 0) {
            _serviceContextHolder = ServiceContext::make();
            makeKClientsWithLockers(state.threads);
            _cv.notify_all();
        } else {
            _cv.wait(ul, [&] { return clients.size() == size_t(state.threads); });
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

BENCHMARK_DEFINE_F(LockManagerTest, BM_LockUnlock_Mutex)(benchmark::State& state) {
    static std::mutex mtx;

    for (auto keepRunning : state) {
        std::unique_lock<std::mutex> lk(mtx);
    }
}

BENCHMARK_DEFINE_F(LockManagerTest, BM_LockUnlock_SharedLock_Direct)(benchmark::State& state) {
    static ResourceMutex resMutex("BM_LockUnlock_SharedLock_Direct");

    auto* lockManager = LockManager::get(getServiceContext());
    Locker locker(getServiceContext());

    for (auto keepRunning : state) {
        LockRequest requestDb;
        requestDb.initNew(
            &locker, nullptr /* This lock will not have contention, so don't pass a notifier */);

        lockManager->lock(resMutex.getRid(), &requestDb, MODE_IS);
        lockManager->unlock(&requestDb);
    }
}

BENCHMARK_DEFINE_F(LockManagerTest, BM_LockUnlock_SharedLock_Locker)(benchmark::State& state) {
    static ResourceMutex resMutex("BM_LockUnlock_SharedLock_Locker");

    auto* opCtx = clients[state.thread_index].second.get();
    Locker locker(getServiceContext());

    for (auto keepRunning : state) {
        locker.lock(opCtx, resMutex.getRid(), MODE_IS);
        locker.unlock(resMutex.getRid());
    }
}

BENCHMARK_REGISTER_F(LockManagerTest, BM_LockUnlock_Mutex)->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(LockManagerTest, BM_LockUnlock_SharedLock_Direct)
    ->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(LockManagerTest, BM_LockUnlock_SharedLock_Locker)
    ->ThreadRange(1, kMaxPerfThreads);


}  // namespace
}  // namespace mongo
