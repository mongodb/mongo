/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"
#include <benchmark/benchmark.h>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const int kMaxPerfThreads = 16;  // max number of threads to use for lock perf


class DConcurrencyTest : public benchmark::Fixture {
public:
    /**
     * Returns a vector of Clients of length 'k', each of which has an OperationContext with its
     * lockState set to a DefaultLockerImpl.
     */
    template <typename LockerType>
    void makeKClientsWithLockers(int k) {
        clients.reserve(k);
        for (int i = 0; i < k; ++i) {
            auto client = getGlobalServiceContext()->makeClient(
                str::stream() << "test client for thread " << i);
            auto opCtx = client->makeOperationContext();
            opCtx->releaseLockState();
            opCtx->setLockState(stdx::make_unique<LockerType>());
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
    }

protected:
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients;
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
};

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_StdMutex)(benchmark::State& state) {
    static stdx::mutex mtx;

    for (auto keepRunning : state) {
        stdx::unique_lock<stdx::mutex> lk(mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_ResourceMutexShared)(benchmark::State& state) {
    static Lock::ResourceMutex mtx("testMutex");

    for (auto keepRunning : state) {
        Lock::SharedLock lk(&locker[state.thread_index], mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_ResourceMutexExclusive)(benchmark::State& state) {
    static Lock::ResourceMutex mtx("testMutex");

    for (auto keepRunning : state) {
        Lock::ExclusiveLock lk(&locker[state.thread_index], mtx);
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionIntentSharedLock)(benchmark::State& state) {
    std::unique_ptr<ForceSupportsDocLocking> supportDocLocking;

    if (state.thread_index == 0) {
        makeKClientsWithLockers<DefaultLockerImpl>(state.threads);
        supportDocLocking = std::make_unique<ForceSupportsDocLocking>(true);
    }

    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), "test", MODE_IS);
        Lock::CollectionLock clk(
            clients[state.thread_index].second->lockState(), "test.coll", MODE_IS);
    }

    if (state.thread_index == 0) {
        clients.clear();
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_CollectionIntentExclusiveLock)(benchmark::State& state) {
    std::unique_ptr<ForceSupportsDocLocking> supportDocLocking;

    if (state.thread_index == 0) {
        makeKClientsWithLockers<DefaultLockerImpl>(state.threads);
        supportDocLocking = std::make_unique<ForceSupportsDocLocking>(true);
    }

    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), "test", MODE_IX);
        Lock::CollectionLock clk(
            clients[state.thread_index].second->lockState(), "test.coll", MODE_IX);
    }

    if (state.thread_index == 0) {
        clients.clear();
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_MMAPv1CollectionSharedLock)(benchmark::State& state) {
    std::unique_ptr<ForceSupportsDocLocking> supportDocLocking;

    if (state.thread_index == 0) {
        makeKClientsWithLockers<DefaultLockerImpl>(state.threads);
        supportDocLocking = std::make_unique<ForceSupportsDocLocking>(false);
    }

    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), "test", MODE_IS);
        Lock::CollectionLock clk(
            clients[state.thread_index].second->lockState(), "test.coll", MODE_S);
    }

    if (state.thread_index == 0) {
        clients.clear();
    }
}

BENCHMARK_DEFINE_F(DConcurrencyTest, BM_MMAPv1CollectionExclusiveLock)(benchmark::State& state) {
    std::unique_ptr<ForceSupportsDocLocking> supportDocLocking;

    if (state.thread_index == 0) {
        makeKClientsWithLockers<DefaultLockerImpl>(state.threads);
        supportDocLocking = std::make_unique<ForceSupportsDocLocking>(false);
    }

    for (auto keepRunning : state) {
        Lock::DBLock dlk(clients[state.thread_index].second.get(), "test", MODE_IX);
        Lock::CollectionLock clk(
            clients[state.thread_index].second->lockState(), "test.coll", MODE_X);
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

BENCHMARK_REGISTER_F(DConcurrencyTest, BM_MMAPv1CollectionSharedLock)
    ->ThreadRange(1, kMaxPerfThreads);
BENCHMARK_REGISTER_F(DConcurrencyTest, BM_MMAPv1CollectionExclusiveLock)
    ->ThreadRange(1, kMaxPerfThreads);

}  // namespace
}  // namespace mongo
