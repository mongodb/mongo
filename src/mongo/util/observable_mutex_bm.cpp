// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
