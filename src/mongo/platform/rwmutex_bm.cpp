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

#include "mongo/platform/rwmutex.h"

#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/processinfo.h"

#include <array>
#include <shared_mutex>
#include <vector>

#include <benchmark/benchmark.h>

#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define MONGO_SANITIZER_BENCHMARK_BUILD
#endif

// Only run `ResourceMutex` benchmarks locally since they cause timeouts and other issues in CI.
#define REGISTER_RESOURCE_MUTEX_BENCHMARKS false

namespace mongo {
namespace {

template <typename DataType>
class WriteRarelyRWMutexController {
public:
    explicit WriteRarelyRWMutexController(DataType value) {
        auto lk = _rwMutex.writeLock();
        _data = value;
    }

    auto read() const {
        auto lk = _rwMutex.readLock();
        return _data;
    }

private:
    mutable WriteRarelyRWMutex _rwMutex;
    DataType _data;
};

template <typename DataType>
class SharedMutexController {
public:
    explicit SharedMutexController(DataType value) {
        stdx::unique_lock lk(_mutex);
        _data = value;
    }

    auto read() const {
        std::shared_lock lk(_mutex);  // NOLINT
        return _data;
    }

private:
    mutable std::shared_mutex _mutex;  // NOLINT
    DataType _data;
};

template <typename DataType>
class MutexController {
public:
    explicit MutexController(DataType value) {
        stdx::unique_lock lk(_mutex);
        _data = value;
    }

    auto read() const {
        stdx::unique_lock lk(_mutex);
        return _data;
    }

private:
    mutable stdx::mutex _mutex;
    DataType _data;
};

template <typename DataType>
class RWMutexController {
public:
    explicit RWMutexController(DataType value) {
        stdx::unique_lock lk(_mutex);
        _data = value;
    }

    auto read() const {
        std::shared_lock lk(_mutex);  // NOLINT
        return _data;
    }

private:
    mutable RWMutex _mutex;
    DataType _data;
};

template <typename DataType>
class ResourceMutexController {
public:
    explicit ResourceMutexController(DataType value)
        : _svcCtx(ServiceContext::make()),
          _lockManager(LockManager::get(_svcCtx.get())),
          _locker(_svcCtx.get()),
          _mutex("BM_ResourceMutexController"),
          _data(value) {}

    auto read() {
        LockRequest request;
        request.initNew(&_locker, nullptr /* No need for a notifier since this won't block. */);
        _lockManager->lock(_mutex.getRid(), &request, MODE_IS);
        auto data = _data;
        _lockManager->unlock(&request);
        return data;
    }

private:
    ServiceContext::UniqueServiceContext _svcCtx;
    LockManager* _lockManager;
    Locker _locker;

    Lock::ResourceMutex _mutex;
    DataType _data;
};

template <template <class> class ControllerType>
class RWMutexBm : public benchmark::Fixture {
public:
    void run(benchmark::State& state) {
        for (auto _ : state) {
            benchmark::DoNotOptimize(_controller.read());
        }
    }

private:
    using DataType = uint64_t;
    static constexpr DataType kInitialValue = 0xABCDEF;
    ControllerType<DataType> _controller{kInitialValue};
};

BENCHMARK_TEMPLATE_DEFINE_F(RWMutexBm, WriteRarelyRWMutex, WriteRarelyRWMutexController)
(benchmark::State& s) {
    run(s);
}
BENCHMARK_TEMPLATE_DEFINE_F(RWMutexBm, SharedMutex, SharedMutexController)(benchmark::State& s) {
    run(s);
}
BENCHMARK_TEMPLATE_DEFINE_F(RWMutexBm, Mutex, MutexController)(benchmark::State& s) {
    run(s);
}
BENCHMARK_TEMPLATE_DEFINE_F(RWMutexBm, RWMutex, RWMutexController)(benchmark::State& s) {
    run(s);
}
BENCHMARK_TEMPLATE_DEFINE_F(RWMutexBm, ResourceMutex, ResourceMutexController)
(benchmark::State& s) {
    run(s);
}

const auto kMaxThreads = ProcessInfo::getNumLogicalCores() * 2;
BENCHMARK_REGISTER_F(RWMutexBm, WriteRarelyRWMutex)->ThreadRange(1, kMaxThreads);
BENCHMARK_REGISTER_F(RWMutexBm, SharedMutex)->ThreadRange(1, kMaxThreads);
BENCHMARK_REGISTER_F(RWMutexBm, Mutex)->ThreadRange(1, kMaxThreads);
BENCHMARK_REGISTER_F(RWMutexBm, RWMutex)->ThreadRange(1, kMaxThreads);
#if REGISTER_RESOURCE_MUTEX_BENCHMARKS
BENCHMARK_REGISTER_F(RWMutexBm, ResourceMutex)->ThreadRange(1, kMaxThreads);
#endif

class RWMutexStressBm : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        auto threads = state.range(0);
        auto readers = state.range(1);

        _stopped.store(false);
        _pendingThreads.store(threads);
        while (threads--) {
            _threads.emplace_back([&] { _workerBody(threads <= readers); });
        }

        auto pending = _pendingThreads.load();
        while (pending > 0) {
            pending = _pendingThreads.wait(pending);
        }
    }

    void TearDown(benchmark::State&) override {
        _stopped.store(true);
        _stopped.notifyAll();
        for (auto& thread : _threads) {
            thread.join();
        }
        _threads.clear();
    }

    void run(benchmark::State& state) {
        for (auto _ : state) {
            auto lk = _rwMutex.writeLock();
        }
    }

private:
    void _workerBody(bool isReader) {
        // Initialize thread-local state for all worker threads, regardless of what they do next.
        {
            auto readLock = _rwMutex.readLock();
        }

        if (_pendingThreads.subtractAndFetch(1) == 0) {
            _pendingThreads.notifyAll();
        }

        if (isReader) {
            while (!_stopped.load()) {
                // This is one of the readers, who will keep acquiring and releasing a read lock.
                auto readLock = _rwMutex.readLock();
            }
        } else {
            _stopped.wait(false);
        }
    }

    WaitableAtomic<bool> _stopped;
    WaitableAtomic<uint32_t> _pendingThreads;

    std::vector<stdx::thread> _threads;

    WriteRarelyRWMutex _rwMutex;
};

BENCHMARK_DEFINE_F(RWMutexStressBm, Write)(benchmark::State& state) {
    run(state);
}
BENCHMARK_REGISTER_F(RWMutexStressBm, Write)->Apply([](auto* b) {
/**
 * Run this in a diminished "correctness check" mode with sanitizers since they do not support a
 * large number of threads.
 */
#ifdef MONGO_SANITIZER_BENCHMARK_BUILD
    constexpr std::array threadCounts{1};
    constexpr std::array readerCounts{0, 1};
#else
    constexpr std::array threadCounts{1, 10, 100, 1000, 10000, 20000};
    constexpr std::array readerCounts{0, 1, 2, 4, 8, 16, 32, 64};
#endif

    b->ArgNames({"Threads", "Readers"});

    for (auto tc : threadCounts) {
        for (auto rc : readerCounts) {
            if (rc > tc)
                break;
            b->Args({tc, rc});
        }
    }
});

}  // namespace
}  // namespace mongo
