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

#include <functional>
#include <list>
#include <memory>

#include <benchmark/benchmark.h>

#include "mongo/platform/atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/lock_free_read_list.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/time_support.h"

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
        stdx::lock_guard lk(_mutex);
        return _list.insert(_list.begin(), data);
    }

    void remove(Iterator iterator) {
        stdx::lock_guard lk(_mutex);
        _list.erase(iterator);
    }

    void forEach(std::function<void(T)> callback) const {
        stdx::lock_guard lk(_mutex);
        for (auto it = _list.begin(); it != _list.end(); ++it) {
            callback(*it);
        }
    }

private:
    mutable stdx::mutex _mutex;
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
        stdx::lock_guard lk(_mutex);
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
        stdx::lock_guard lk(_mutex);
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
            stdx::this_thread::yield();
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

    stdx::mutex _mutex;
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
