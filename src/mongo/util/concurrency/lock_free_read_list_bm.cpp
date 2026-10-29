// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/concurrency/lock_free_read_list.h"

#include "mongo/platform/atomic.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using DataType = size_t;

/**
 * This is the baseline and an alternative for the lock-free-read list implementation. It provides
 * the same APIs, while using an exclusive lock to serialize accessing and modifying the list.
 */
template <typename T>
class MutexListController {
public:
    using ListType = std::list<T>;
    using Iterator = typename ListType::iterator;

    Iterator add(T data) {
        std::lock_guard lk(_mutex);
        return _list.insert(_list.begin(), data);
    }

    void remove(Iterator iterator) {
        std::lock_guard lk(_mutex);
        _list.erase(iterator);
    }

    void forEach(std::function<void(T)> callback) const {
        std::lock_guard lk(_mutex);
        for (auto it = _list.begin(); it != _list.end(); ++it) {
            callback(*it);
        }
    }

private:
    mutable std::mutex _mutex;
    ListType _list;
};

template <typename T>
class LockFreeReadListController {
public:
    using ListType = LockFreeReadList<T>;
    using Iterator = typename ListType::Entry*;

    Iterator add(T data) {
        return _list.add(data);
    }

    void remove(Iterator iterator) {
        _list.remove(iterator);
    }

    void forEach(std::function<void(T)> callback) const {
        for (auto cursor = _list.getCursor(); !!cursor; cursor.next()) {
            callback(cursor.value());
        }
    }

private:
    ListType _list;
};

template <typename ControllerType>
class SynchronizedListBm : public benchmark::Fixture {
public:
    static constexpr size_t kListLength = 2048;

    void setUp() {
        std::lock_guard lk(_mutex);
        if (_threads++ == 0) {
            // Make sure the list is not empty when we start running the benchmark. These are
            // basically immutable entries that are never removed while the benchmark is running.
            for (size_t i = 0; i < kListLength; ++i) {
                _controller.add(_counter.fetchAndAdd(1));
            }
            _totalObserved.store(0);
            _totalUpdated.store(0);
            _startedAt = _tickSource->getTicks();
        }
    }

    void finalize(benchmark::State& state) {
        std::lock_guard lk(_mutex);
        if (--_threads == 0) {
            const auto stoppedAt = _tickSource->getTicks();
            const auto microseconds = durationCount<Microseconds>(
                _tickSource->ticksTo<Microseconds>(stoppedAt - _startedAt));
            const auto observed = _totalObserved.load();
            const auto updated = _totalUpdated.load();
            state.counters["Observed"] = observed;
            state.counters["Updated"] = updated;
            state.counters["ObservePerSec"] =
                benchmark::Counter(observed * 1E6 / microseconds, benchmark::Counter::kIsRate);
            state.counters["UpdatePerSec"] =
                benchmark::Counter(updated * 1E6 / microseconds, benchmark::Counter::kIsRate);
        }
    }

    void Run(benchmark::State& state) override {
        setUp();
        if (state.thread_index % 2) {
            Iterate(state);
        } else {
            AddAndRemove(state);
        }
        finalize(state);
    }

    void Iterate(benchmark::State& state) {
        auto callback = [] {
            // Mimic processing of list entries.
            std::vector<int> fibs{0, 1};
            for (int i = 1; i < 10; i++) {
                fibs.push_back(fibs[i] + fibs[i - 1]);
            }
            return fibs;
        };

        size_t observed = 0;
        for (auto _ : state) {
            _controller.forEach([&](auto) mutable {
                state.PauseTiming();
                ++observed;
                benchmark::DoNotOptimize(callback());
                state.ResumeTiming();
            });
        }

        _totalObserved.fetchAndAdd(observed);
    }

    void AddAndRemove(benchmark::State& state) {
        size_t updates = 0;
        for (auto _ : state) {
            auto it = _controller.add(_counter.fetchAndAdd(1));
            state.PauseTiming();
            std::this_thread::yield();
            state.ResumeTiming();
            _controller.remove(it);
            updates += 2;
        }

        _totalUpdated.fetchAndAdd(updates);
    }

private:
    std::unique_ptr<TickSource> _tickSource = makeSystemTickSource();
    ControllerType _controller;

    Atomic<DataType> _counter;
    Atomic<size_t> _totalObserved;
    Atomic<size_t> _totalUpdated;

    std::mutex _mutex;
    size_t _threads;
    TickSource::Tick _startedAt;
};

BENCHMARK_TEMPLATE_DEFINE_F(SynchronizedListBm,
                            LockFreeReadList,
                            LockFreeReadListController<DataType>)
(benchmark::State& st) {
    Run(st);
}
BENCHMARK_TEMPLATE_DEFINE_F(SynchronizedListBm, MutexList, MutexListController<DataType>)
(benchmark::State& st) {
    Run(st);
}

const auto kMaxThreads = ProcessInfo::getNumLogicalCores() * 4;
BENCHMARK_REGISTER_F(SynchronizedListBm, LockFreeReadList)->ThreadRange(2, kMaxThreads);
BENCHMARK_REGISTER_F(SynchronizedListBm, MutexList)->ThreadRange(2, kMaxThreads);

}  // namespace
}  // namespace mongo
