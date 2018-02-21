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

#include <boost/optional.hpp>
#include <deque>
#include <list>
#include <queue>
#include <stack>

#include "mongo/db/operation_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace producer_consumer_queue_detail {

/**
 * The default cost function for the producer consumer queue.
 *
 * By default, all items in the queue have equal weight.
 */
struct DefaultCostFunction {
    template <typename T>
    size_t operator()(const T&) const {
        return 1;
    }
};

// Various helpers to tighten down whether the args getting passed are valid interruption args.
//
// Whatever the caller passes in the interruption args, they need to be invocable on one of
// these helpers. std::is_invocable would do the job in C++17
constexpr std::false_type areInterruptionArgsHelper(...) {
    return {};
}

constexpr std::true_type areInterruptionArgsHelper(OperationContext*) {
    return {};
}

constexpr std::true_type areInterruptionArgsHelper(OperationContext*, Milliseconds) {
    return {};
}

constexpr std::true_type areInterruptionArgsHelper(OperationContext*, Date_t) {
    return {};
}

constexpr std::true_type areInterruptionArgsHelper(Milliseconds) {
    return {};
}

constexpr std::true_type areInterruptionArgsHelper(Date_t) {
    return {};
}

template <typename U, typename... InterruptionArgs>
constexpr auto areInterruptionArgs(U&& u, InterruptionArgs&&... args) {
    return areInterruptionArgsHelper(std::forward<U>(u), std::forward<InterruptionArgs>(args)...);
}

constexpr std::true_type areInterruptionArgs() {
    return {};
}

}  // namespace producer_consumer_queue_detail

/**
 * A bounded, blocking, thread safe, cost parametrizable, single producer, multi-consumer queue.
 *
 * Properties:
 *   bounded - the queue can be limited in the number of items it can hold
 *   blocking - when the queue is full, or has no entries, callers block
 *   thread safe - the queue can be accessed safely from multiple threads at the same time
 *   cost parametrizable - the cost of items in the queue need not be equal. I.e. your items could
 *                          be discrete byte buffers and the queue depth measured in bytes, so that
 *                          the queue could hold one large buffer, or many smaller ones
 *   single producer - Only one thread may push work into the queue
 *   multi-consumer - Any number of threads may pop work out of the queue
 *
 * Interruptibility:
 *   All of the blocking methods on this type allow for 6 kinds of interruptibility. The matrix is
 *   parameterized by (void|OperationContext*)|(void|Milliseconds|Date_t). These provide different
 *   kinds of waiting based on whether the method should be interruptible via opCtx, and then
 *   whether they should timeout via deadline or duration
 *
 *   A contrived example: pcq.pop(opCtx, Minutes(1)) would be warranted if:
 *     - The caller is blocking on a client thread. (opCtx)
 *     - The caller needs to act periodically on inactivity. (the duration)
 *
 * Exceptions include:
 *   timeouts
 *     ErrorCodes::ExceededTimeLimit exceptions
 *   opCtx interrupts
 *     ErrorCodes::Interrupted exceptions
 *   closure of queue endpoints
 *     ErrorCodes::ProducerConsumerQueueEndClosed
 *   pushes with batches that exceed the max queue size
 *     ErrorCodes::ProducerConsumerQueueBatchTooLarge
 *
 * Cost Function:
 *   The cost function must have a call operator which takes a const T& and returns the cost in
 *   size_t units. It must be pure across moves for a given T and never return zero. The intent of
 *   the cost function is to express the kind of bounds the queue provides, rather than to
 *   specialize behavior for a type. I.e. you should not specialize the default cost function and
 *   the cost function should always be explicit in the type.
 */
template <typename T, typename CostFunc = producer_consumer_queue_detail::DefaultCostFunction>
class ProducerConsumerQueue {

public:
    // By default the queue depth is unlimited
    ProducerConsumerQueue()
        : ProducerConsumerQueue(std::numeric_limits<size_t>::max(), CostFunc{}) {}

    // Or it can be measured in whatever units your size function returns
    explicit ProducerConsumerQueue(size_t size) : ProducerConsumerQueue(size, CostFunc{}) {}

    // If your cost function has meaningful state, you may also pass a non-default constructed
    // instance
    explicit ProducerConsumerQueue(size_t size, CostFunc costFunc)
        : _max(size), _costFunc(std::move(costFunc)) {}

    ProducerConsumerQueue(const ProducerConsumerQueue&) = delete;
    ProducerConsumerQueue& operator=(const ProducerConsumerQueue&) = delete;

    ProducerConsumerQueue(ProducerConsumerQueue&&) = delete;
    ProducerConsumerQueue& operator=(ProducerConsumerQueue&&) = delete;

    ~ProducerConsumerQueue() {
        invariant(!_producerWants);
        invariant(!_consumers);
    }

    // Pushes the passed T into the queue
    //
    // Leaves T unchanged if an interrupt exception is thrown while waiting for space
    template <
        typename... InterruptionArgs,
        typename = std::enable_if_t<decltype(producer_consumer_queue_detail::areInterruptionArgs(
            std::declval<InterruptionArgs>()...))::value>>
    void push(T&& t, InterruptionArgs&&... interruptionArgs) {
        _pushRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            auto cost = _invokeCostFunc(t, lk);
            uassert(ErrorCodes::ProducerConsumerQueueBatchTooLarge,
                    str::stream() << "cost of item (" << cost
                                  << ") larger than maximum queue size ("
                                  << _max
                                  << ")",
                    cost <= _max);

            _waitForSpace(lk, cost, std::forward<InterruptionArgs>(interruptionArgs)...);
            _push(lk, std::move(t));
        });
    }

    // Pushes all Ts into the queue
    //
    // Blocks until all of the Ts can be pushed at once
    //
    // StartIterator must be ForwardIterator
    //
    // Leaves the values underneath the iterators unchanged if an interrupt exception is thrown
    // while waiting for space
    //
    // Lifecycle methods of T must not throw if you want to use this method, as there's no obvious
    // mechanism to see what was and was not pushed if those do throw
    template <
        typename StartIterator,
        typename EndIterator,
        typename... InterruptionArgs,
        typename = std::enable_if_t<decltype(producer_consumer_queue_detail::areInterruptionArgs(
            std::declval<InterruptionArgs>()...))::value>>
    void pushMany(StartIterator start, EndIterator last, InterruptionArgs&&... interruptionArgs) {
        return _pushRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            size_t cost = 0;
            for (auto iter = start; iter != last; ++iter) {
                cost += _invokeCostFunc(*iter, lk);
            }

            uassert(ErrorCodes::ProducerConsumerQueueBatchTooLarge,
                    str::stream() << "cost of items in batch (" << cost
                                  << ") larger than maximum queue size ("
                                  << _max
                                  << ")",
                    cost <= _max);

            _waitForSpace(lk, cost, std::forward<InterruptionArgs>(interruptionArgs)...);

            for (auto iter = start; iter != last; ++iter) {
                _push(lk, std::move(*iter));
            }
        });
    }

    // Attempts a non-blocking push of a value
    //
    // Leaves T unchanged if it fails
    bool tryPush(T&& t) {
        return _pushRunner(
            [&](stdx::unique_lock<stdx::mutex>& lk) { return _tryPush(lk, std::move(t)); });
    }

    // Pops one T out of the queue
    template <
        typename... InterruptionArgs,
        typename = std::enable_if_t<decltype(producer_consumer_queue_detail::areInterruptionArgs(
            std::declval<InterruptionArgs>()...))::value>>
    T pop(InterruptionArgs&&... interruptionArgs) {
        return _popRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            _waitForNonEmpty(lk, std::forward<InterruptionArgs>(interruptionArgs)...);
            return _pop(lk);
        });
    }

    // Waits for at least one item in the queue, then pops items out of the queue until it would
    // block
    //
    // OutputIterator must not throw on move assignment to *iter or popped values may be lost
    // TODO: add sfinae to check to enforce
    //
    // Returns the cost value of the items extracted, along with the updated output iterator
    template <
        typename OutputIterator,
        typename... InterruptionArgs,
        typename = std::enable_if_t<decltype(producer_consumer_queue_detail::areInterruptionArgs(
            std::declval<InterruptionArgs>()...))::value>>
    std::pair<size_t, OutputIterator> popMany(OutputIterator iterator,
                                              InterruptionArgs&&... interruptionArgs) {
        return popManyUpTo(_max, iterator, std::forward<InterruptionArgs>(interruptionArgs)...);
    }

    // Waits for at least one item in the queue, then pops items out of the queue until it would
    // block, or we've exceeded our budget
    //
    // OutputIterator must not throw on move assignment to *iter or popped values may be lost
    // TODO: add sfinae to check to enforce
    //
    // Returns the cost value of the items extracted, along with the updated output iterator
    template <
        typename OutputIterator,
        typename... InterruptionArgs,
        typename = std::enable_if_t<decltype(producer_consumer_queue_detail::areInterruptionArgs(
            std::declval<InterruptionArgs>()...))::value>>
    std::pair<size_t, OutputIterator> popManyUpTo(size_t budget,
                                                  OutputIterator iterator,
                                                  InterruptionArgs&&... interruptionArgs) {
        return _popRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            size_t cost = 0;

            _waitForNonEmpty(lk, std::forward<InterruptionArgs>(interruptionArgs)...);

            while (auto out = _tryPop(lk)) {
                cost += _invokeCostFunc(*out, lk);
                *iterator = std::move(*out);
                ++iterator;

                if (cost >= budget) {
                    break;
                }
            }

            return std::make_pair(cost, iterator);
        });
    }

    // Attempts a non-blocking pop of a value
    boost::optional<T> tryPop() {
        return _popRunner([&](stdx::unique_lock<stdx::mutex>& lk) { return _tryPop(lk); });
    }

    // Closes the producer end. Consumers will continue to consume until the queue is exhausted, at
    // which time they will begin to throw with an interruption dbexception
    void closeProducerEnd() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        _producerEndClosed = true;

        _notifyIfNecessary(lk);
    }

    // Closes the consumer end. This causes all callers to throw with an interruption dbexception
    void closeConsumerEnd() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        _consumerEndClosed = true;
        _producerEndClosed = true;

        _notifyIfNecessary(lk);
    }

    // TEST ONLY FUNCTIONS

    // Returns the current depth of the queue in CostFunction units
    size_t sizeForTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        return _current;
    }

    // Returns true if the queue is empty
    bool emptyForTest() const {
        return sizeForTest() == 0;
    }

private:
    size_t _invokeCostFunc(const T& t, WithLock) {
        auto cost = _costFunc(t);
        invariant(cost);
        return cost;
    }

    void _checkProducerClosed(WithLock) {
        uassert(
            ErrorCodes::ProducerConsumerQueueEndClosed, "Producer end closed", !_producerEndClosed);
        uassert(
            ErrorCodes::ProducerConsumerQueueEndClosed, "Consumer end closed", !_consumerEndClosed);
    }

    void _checkConsumerClosed(WithLock) {
        uassert(
            ErrorCodes::ProducerConsumerQueueEndClosed, "Consumer end closed", !_consumerEndClosed);
        uassert(ErrorCodes::ProducerConsumerQueueEndClosed,
                "Producer end closed and values exhausted",
                !(_producerEndClosed && _queue.empty()));
    }

    void _notifyIfNecessary(WithLock) {
        // If we've closed the consumer end, or if the production end is closed and we've exhausted
        // the queue, wake everyone up and get out of here
        if (_consumerEndClosed || (_queue.empty() && _producerEndClosed)) {
            if (_consumers) {
                _condvarConsumer.notify_all();
            }

            if (_producerWants) {
                _condvarProducer.notify_one();
            }

            return;
        }

        // If a producer is queued, and we have enough space for it to push its work
        if (_producerWants && _current + _producerWants <= _max) {
            _condvarProducer.notify_one();

            return;
        }

        // If we have consumers and anything in the queue, notify consumers
        if (_consumers && _queue.size()) {
            _condvarConsumer.notify_one();

            return;
        }
    }

    template <typename Callback>
    auto _pushRunner(Callback&& cb) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        _checkProducerClosed(lk);

        const auto guard = MakeGuard([&] { _notifyIfNecessary(lk); });

        return cb(lk);
    }

    template <typename Callback>
    auto _popRunner(Callback&& cb) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        _checkConsumerClosed(lk);

        const auto guard = MakeGuard([&] { _notifyIfNecessary(lk); });

        return cb(lk);
    }

    bool _tryPush(WithLock wl, T&& t) {
        size_t cost = _invokeCostFunc(t, wl);
        if (_current + cost <= _max) {
            _queue.emplace(std::move(t));
            _current += cost;
            return true;
        }

        return false;
    }

    void _push(WithLock wl, T&& t) {
        size_t cost = _invokeCostFunc(t, wl);
        invariant(_current + cost <= _max);

        _queue.emplace(std::move(t));
        _current += cost;
    }

    boost::optional<T> _tryPop(WithLock wl) {
        boost::optional<T> out;

        if (!_queue.empty()) {
            out.emplace(std::move(_queue.front()));
            _queue.pop();
            _current -= _invokeCostFunc(*out, wl);
        }

        return out;
    }

    T _pop(WithLock wl) {
        invariant(_queue.size());

        auto t = std::move(_queue.front());
        _queue.pop();

        _current -= _invokeCostFunc(t, wl);

        return t;
    }

    template <typename... InterruptionArgs>
    void _waitForSpace(stdx::unique_lock<stdx::mutex>& lk,
                       size_t cost,
                       InterruptionArgs&&... interruptionArgs) {
        invariant(!_producerWants);

        _producerWants = cost;
        const auto guard = MakeGuard([&] { _producerWants = 0; });

        _waitFor(lk,
                 _condvarProducer,
                 [&] {
                     _checkProducerClosed(lk);
                     return _current + cost <= _max;
                 },
                 std::forward<InterruptionArgs>(interruptionArgs)...);
    }

    template <typename... InterruptionArgs>
    void _waitForNonEmpty(stdx::unique_lock<stdx::mutex>& lk,
                          InterruptionArgs&&... interruptionArgs) {

        _consumers++;
        const auto guard = MakeGuard([&] { _consumers--; });

        _waitFor(lk,
                 _condvarConsumer,
                 [&] {
                     _checkConsumerClosed(lk);
                     return _queue.size();
                 },
                 std::forward<InterruptionArgs>(interruptionArgs)...);
    }

    template <typename Callback>
    void _waitFor(stdx::unique_lock<stdx::mutex>& lk,
                  stdx::condition_variable& condvar,
                  Callback&& pred,
                  OperationContext* opCtx) {
        opCtx->waitForConditionOrInterrupt(condvar, lk, pred);
    }

    template <typename Callback>
    void _waitFor(stdx::unique_lock<stdx::mutex>& lk,
                  stdx::condition_variable& condvar,
                  Callback&& pred) {
        condvar.wait(lk, pred);
    }

    template <typename Callback>
    void _waitFor(stdx::unique_lock<stdx::mutex>& lk,
                  stdx::condition_variable& condvar,
                  Callback&& pred,
                  OperationContext* opCtx,
                  Date_t deadline) {
        uassert(ErrorCodes::ExceededTimeLimit,
                "exceeded timeout",
                opCtx->waitForConditionOrInterruptUntil(condvar, lk, deadline, pred));
    }

    template <typename Callback>
    void _waitFor(stdx::unique_lock<stdx::mutex>& lk,
                  stdx::condition_variable& condvar,
                  Callback&& pred,
                  Date_t deadline) {
        uassert(ErrorCodes::ExceededTimeLimit,
                "exceeded timeout",
                condvar.wait_until(lk, deadline.toSystemTimePoint(), pred));
    }

    template <typename Callback>
    void _waitFor(stdx::unique_lock<stdx::mutex>& lk,
                  stdx::condition_variable& condvar,
                  Callback&& pred,
                  OperationContext* opCtx,
                  Milliseconds duration) {
        uassert(ErrorCodes::ExceededTimeLimit,
                "exceeded timeout",
                opCtx->waitForConditionOrInterruptFor(condvar, lk, duration, pred));
    }

    template <typename Callback>
    void _waitFor(stdx::unique_lock<stdx::mutex>& lk,
                  stdx::condition_variable& condvar,
                  Callback&& pred,
                  Milliseconds duration) {
        uassert(ErrorCodes::ExceededTimeLimit,
                "exceeded timeout",
                condvar.wait_for(lk, duration.toSystemDuration(), pred));
    }

    mutable stdx::mutex _mutex;
    stdx::condition_variable _condvarConsumer;
    stdx::condition_variable _condvarProducer;

    // Max size of the queue
    const size_t _max;

    // User's cost function
    CostFunc _costFunc;

    // Current size of the queue
    size_t _current = 0;

    std::queue<T> _queue;

    // Counter for consumers in the queue
    size_t _consumers = 0;

    // Size of batch the blocking producer wants to insert
    size_t _producerWants = 0;

    // Flags that we're shutting down the queue
    bool _consumerEndClosed = false;
    bool _producerEndClosed = false;
};

}  // namespace mongo
