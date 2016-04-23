/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>

#include <boost/optional.hpp>
#include <utility>
#include <vector>

#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool_interface.h"

namespace mongo {
namespace executor {

/**
 * A mock class mimicking TaskExecutor::CallbackState, does nothing.
 */
class MockCallbackState final : public TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

inline TaskExecutor::CallbackHandle makeCallbackHandle() {
    return TaskExecutor::CallbackHandle(std::make_shared<MockCallbackState>());
}

/**
 * Simple future-like utility for waiting for the result of startCommand.
 */
template <typename T>
class Deferred {
public:
    template <typename... Args>
    void emplace(Args&&... args) {
        _emplace(_state.get(), std::forward<Args>(args)...);
    }

    T& get() {
        return _get(_state.get());
    }

    bool hasCompleted() {
        stdx::unique_lock<stdx::mutex> lk(_state->mtx);
        return _state->thing.is_initialized();
    }

    template <typename Continuation>
    auto then(ThreadPoolInterface* pool, Continuation&& continuation)
        -> Deferred<decltype(continuation(std::declval<Deferred<T>>().get()))> {
        // XXX: The ugliness of the above type signature is because you can't refer to 'this' in
        // a template parameter, at least on g++-4.8.2.
        auto state = _state;
        Deferred<decltype(continuation(get()))> thenDeferred;

        pool->schedule([this, thenDeferred, continuation, state]() mutable {
            thenDeferred.emplace(continuation(_get(state.get())));
        });
        return thenDeferred;
    }

private:
    struct State {
        stdx::mutex mtx;
        stdx::condition_variable cv;
        boost::optional<T> thing;
    };

    template <typename... Args>
    void _emplace(State* state, Args&&... args) {
        stdx::lock_guard<stdx::mutex> lk(_state->mtx);
        invariant(!state->thing.is_initialized());
        state->thing.emplace(std::forward<T>(args)...);
        state->cv.notify_one();
    }

    T& _get(State* state) {
        stdx::unique_lock<stdx::mutex> lk(state->mtx);
        state->cv.wait(lk, [state] { return state->thing.is_initialized(); });
        return *state->thing;
    }

private:
    std::shared_ptr<State> _state = std::make_shared<State>();
};

namespace helpers {

template <typename T>
static Deferred<std::vector<T>> collect(std::vector<Deferred<T>>& ds, ThreadPoolInterface* pool) {
    Deferred<std::vector<T>> out;
    struct CollectState {
        // hack to avoid requiring U to be default constructible.
        std::vector<boost::optional<T>> mem{};
        std::size_t numFinished = 0;
        std::size_t goal = 0;
        stdx::mutex mtx;
    };

    auto collectState = std::make_shared<CollectState>();
    collectState->goal = ds.size();
    collectState->mem.resize(collectState->goal);

    for (std::size_t i = 0; i < ds.size(); ++i) {
        ds[i].then(pool, [collectState, out, i](T res) mutable {
            // The bool return is unused.
            stdx::lock_guard<stdx::mutex> lk(collectState->mtx);
            collectState->mem[i] = std::move(res);

            // If we're done.
            if (collectState->goal == ++collectState->numFinished) {
                std::vector<T> outInitialized;
                outInitialized.reserve(collectState->mem.size());
                for (auto&& mem_entry : collectState->mem) {
                    outInitialized.emplace_back(std::move(*mem_entry));
                }
                out.emplace(outInitialized);
            }
            return true;
        });
    }
    return out;
}

}  // namespace helpers

}  // namespace executor
}  // namespace mongo
