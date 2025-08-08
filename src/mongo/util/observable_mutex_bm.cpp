/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/observable_mutex.h"

#include "mongo/util/duration.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class ExclusiveLocker {
public:
    template <typename MutexType>
    void lock(MutexType& m) {
        m.lock();
    }

    template <typename MutexType>
    void unlock(MutexType& m) {
        m.unlock();
    }
};

class SharedLocker {
public:
    template <typename MutexType>
    void lock(MutexType& m) {
        m.lock_shared();
    }

    template <typename MutexType>
    void unlock(MutexType& m) {
        m.unlock_shared();
    }
};

template <typename MutexType, typename LockerType>
class BM_Mutex : public benchmark::Fixture {
public:
    template <typename WorkCallback>
    void run(benchmark::State& state, WorkCallback doWork) {
        for (auto _ : state) {
            _locker.lock(_mutex);
            doWork();
            _locker.unlock(_mutex);
        }
    }

    void runLockThenUnlock(benchmark::State& state) {
        run(state, [] {});
    }

    void runLockSleepThenUnlock(benchmark::State& state) {
        run(state, [this] { sleepFor(kSleepDuration); });
    }

private:
    const Microseconds kSleepDuration{1};
    MutexType _mutex;
    LockerType _locker;
};

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex, MutexLockThenUnlock, std::mutex, ExclusiveLocker)
(benchmark::State& s) {
    runLockThenUnlock(s);
}

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex,
                            ObsMutexLockThenUnlock,
                            ObservableExclusiveMutex,
                            ExclusiveLocker)
(benchmark::State& s) {
    runLockThenUnlock(s);
}

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex, SharedMutexLockThenUnlock, std::shared_mutex, SharedLocker)
(benchmark::State& s) {
    runLockThenUnlock(s);
}

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex,
                            ObsSharedMutexLockThenUnlock,
                            ObservableSharedMutex,
                            SharedLocker)
(benchmark::State& s) {
    runLockThenUnlock(s);
}

BENCHMARK_REGISTER_F(BM_Mutex, MutexLockThenUnlock);
BENCHMARK_REGISTER_F(BM_Mutex, ObsMutexLockThenUnlock);
BENCHMARK_REGISTER_F(BM_Mutex, SharedMutexLockThenUnlock);
BENCHMARK_REGISTER_F(BM_Mutex, ObsSharedMutexLockThenUnlock);

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex, MutexLockSleepUnlock, std::mutex, ExclusiveLocker)
(benchmark::State& s) {
    runLockSleepThenUnlock(s);
}

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex,
                            ObsMutexLockSleepUnlock,
                            ObservableExclusiveMutex,
                            ExclusiveLocker)
(benchmark::State& s) {
    runLockSleepThenUnlock(s);
}

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex, SharedMutexLockSleepUnlock, std::shared_mutex, SharedLocker)
(benchmark::State& s) {
    runLockSleepThenUnlock(s);
}

BENCHMARK_TEMPLATE_DEFINE_F(BM_Mutex,
                            ObsSharedMutexLockSleepUnlock,
                            ObservableSharedMutex,
                            SharedLocker)
(benchmark::State& s) {
    runLockSleepThenUnlock(s);
}

#ifdef MONGO_SANITIZER_BENCHMARK_BUILD
const auto kMaxThreads = 4;
#else
const auto kMaxThreads = 2 * ProcessInfo::getNumAvailableCores();
#endif

BENCHMARK_REGISTER_F(BM_Mutex, MutexLockSleepUnlock)->ThreadRange(2, kMaxThreads);
BENCHMARK_REGISTER_F(BM_Mutex, ObsMutexLockSleepUnlock)->ThreadRange(2, kMaxThreads);
BENCHMARK_REGISTER_F(BM_Mutex, SharedMutexLockSleepUnlock)->ThreadRange(2, kMaxThreads);
BENCHMARK_REGISTER_F(BM_Mutex, ObsSharedMutexLockSleepUnlock)->ThreadRange(2, kMaxThreads);

}  // namespace
}  // namespace mongo
