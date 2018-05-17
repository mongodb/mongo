/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <deque>
#include <vector>

#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {

/**
 * This is a thread safe execution primitive for running jobs against an executor with mutual
 * exclusion and queuing by key.
 *
 * Features:
 *   Keyed - Tasks are submitted under a key.  The keys serve to prevent tasks for a given key from
 *           executing simultaneously.  Tasks submitted under different keys may run concurrently.
 *
 *   Queued - If a task is submitted for a key and another task is already running for that key, it
 *            is queued.  I.e. tasks are run in FIFO order for a key.
 *
 *   Thread Safe - This is a thread safe type.  Any number of callers may invoke the public api
 *                 methods simultaneously.
 *
 * Special Enhancements:
 *   onCurrentTasksDrained- Invoking this method for a key allows a caller to wait until all of the
 *                          currently queued tasks for that key have completed.
 *
 *   onAllCurrentTasksDrained- Invoking this method allows a caller to wait until all of the
 *                             currently queued tasks for all key have completed.
 *
 *   KeyedExecutorRetry - Throwing or returning KeyedExecutorRetry in a task will cause the task to
 *                        be requeued immediately into the executor and retain its place in the
 *                        queue.
 *
 * The template arguments to the type include the Key we wish to schedule under, and arguments that
 * are passed through to stdx::unordered_map (I.e. Hash, KeyEqual, Allocator, etc).
 *
 * It is a programming error to destroy this type with tasks still in the queue.  Clean shutdown can
 * be effected by ceasing to queue new work, running tasks which can fail early and waiting on
 * onAllCurrentTasksDrained.
 */
template <typename Key, typename... MapArgs>
class KeyedExecutor {
    // We hold a deque per key.  Each entry in the deque represents a task we'll eventually execute
    // and a list of callers who need to be notified after it completes.
    using Deque = std::deque<std::vector<SharedPromise<void>>>;

    using Map = stdx::unordered_map<Key, Deque, MapArgs...>;

public:
    explicit KeyedExecutor(OutOfLineExecutor* executor) : _executor(executor) {}

    KeyedExecutor(const KeyedExecutor&) = delete;
    KeyedExecutor& operator=(const KeyedExecutor&) = delete;

    KeyedExecutor(KeyedExecutor&&) = delete;
    KeyedExecutor& operator=(KeyedExecutor&&) = delete;

    ~KeyedExecutor() {
        invariant(_map.empty());
    }

    /**
     * Executes the callback on the associated executor.  If another task is currently running for a
     * given key, queues until that task is finished.
     */
    template <typename Callback>
    Future<FutureContinuationResult<Callback>> execute(const Key& key, Callback&& cb) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        typename Map::iterator iter;
        bool wasInserted;
        std::tie(iter, wasInserted) = _map.emplace(
            std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());

        if (wasInserted) {
            // If there wasn't a key, we're the first job, just run immediately
            iter->second.emplace_back();

            // Drop the lock before running execute to avoid deadlocks
            lk.unlock();
            return _execute(iter, std::forward<Callback>(cb));
        }

        // If there's already a key, we queue up our execution behind it
        auto future =
            _onCleared(lk, iter->second).then([this, iter, cb] { return _execute(iter, cb); });

        // Create a new set of promises for callers who rely on our readiness
        iter->second.emplace_back();

        return future;
    }

    /**
     * Returns a future which becomes ready when all queued tasks for a given key have completed.
     *
     * Note that this doesn't prevent other tasks from queueing and the readiness of this future
     * says nothing about the execution of those tasks queued after this call.
     */
    Future<void> onCurrentTasksDrained(const Key& key) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        auto iter = _map.find(key);

        if (iter == _map.end()) {
            // If there wasn't a key, we're already cleared
            return Future<void>::makeReady();
        }

        return _onCleared(lk, iter->second);
    }

    /**
     * Returns a future which becomes ready when all queued tasks for all keys have completed.
     *
     * Note that this doesn't prevent other tasks from queueing and the readiness of this future
     * says nothing about the execution of those tasks queued after this call.
     */
    Future<void> onAllCurrentTasksDrained() {
        // This latch works around a current lack of whenAll.  We have less need of a complicated
        // type however (because our only failure mode is broken promise, a programming error here,
        // and because we only need to handle void and can collapse).
        struct Latch {
            ~Latch() {
                promise.emplaceValue();
            }

            Promise<void> promise;
        };

        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_map.empty()) {
            // If there isn't any state, just return
            return Future<void>::makeReady();
        }

        // We rely on shard_ptr to handle the atomic refcounting before emplacing for us.
        auto latch = std::make_shared<Latch>();
        auto future = latch->promise.getFuture();

        for (auto& pair : _map) {
            _onCleared(lk, pair.second).getAsync([latch](const Status& status) mutable {
                invariant(status.isOK());
                latch.reset();
            });
        }

        return future;
    }

private:
    /**
     * executes and retries if the callback throws/returns KeyedExecutorRetry
     */
    template <typename Callback>
    Future<FutureContinuationResult<Callback>> _executeRetryErrors(Callback&& cb) {
        return _executor->execute(std::forward<Callback>(cb))
            .onError([this, cb](const Status& status) {
                if (status.code() == ErrorCodes::KeyedExecutorRetry) {
                    return _executeRetryErrors(cb);
                }

                return Future<FutureContinuationResult<Callback>>(status);
            });
    };

    template <typename Callback>
    Future<FutureContinuationResult<Callback>> _execute(typename Map::iterator iter,
                                                        Callback&& cb) {
        // First we run until success, or non retry-able error
        return _executeRetryErrors(std::forward<Callback>(cb)).tapAll([this, iter](const auto&) {
            // Then handle clean up
            auto promises = [&] {
                stdx::lock_guard<stdx::mutex> lk(_mutex);

                auto& deque = iter->second;
                auto promises = std::move(deque.front());
                deque.pop_front();

                if (deque.empty()) {
                    _map.erase(iter);
                }

                return promises;
            }();

            // fulfill promises outside the lock
            for (auto& promise : promises) {
                promise.emplaceValue();
            }
        });
    }

    Future<void> _onCleared(WithLock, Deque& deque) {
        invariant(deque.size());

        Promise<void> promise;
        Future<void> future = promise.getFuture();

        deque.back().push_back(promise.share());

        return future;
    }

    stdx::mutex _mutex;
    Map _map;
    OutOfLineExecutor* _executor;
};

}  // namespace mongo
