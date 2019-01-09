
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include <boost/optional.hpp>
#include <deque>
#include <list>
#include <numeric>
#include <queue>
#include <stack>

#include "mongo/db/operation_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/interruptible.h"
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

/**
 * These enums help make the following template specializations a bit more self documenting
 */
enum ProducerKind : bool { MultiProducer = true, SingleProducer = false };
enum ConsumerKind : bool { MultiConsumer = true, SingleConsumer = false };
enum Kind : bool { Multi = true, Single = false };

/**
 * Closes some end of the queue (given by the close pointer to member) on destruction.
 *
 * Also offers operator->() access to the underlying queue.
 *
 * We're only moveable because a copy would cause double closes
 */
template <typename PCQ, void (PCQ::*close)()>
class Closer {
public:
    explicit Closer(const std::shared_ptr<PCQ>& parent) : _parent(parent) {}

    Closer(const Closer&) = delete;
    Closer& operator=(const Closer&) = delete;

    Closer(Closer&&) = default;
    Closer& operator=(Closer&&) = default;

    ~Closer() {
        // We may be moved from
        if (_parent) {
            ((*_parent).*close)();
        }
    }

    const std::shared_ptr<PCQ>& operator->() const {
        return _parent;
    }

private:
    std::shared_ptr<PCQ> _parent;
};

/**
 * This holder provides a pivot for the construction of the Pipe ends of a producer consumer queue
 * (when used in that mode).
 *
 * It holds a Producer or Consumer auto-closer end and is multi or not via the kind
 *
 * On destruction, the Closer constructed from PCQ and close will call closeProducerEnd() or
 * closeConsumerEnd()
 *
 * We take by boolean kind so that we get convertibility from Producer and Consumer Kind.
 */
template <bool kind, typename PCQ, void (PCQ::*close)()>
class Holder;
/**
 * {
 *     // Takes the producer consumer queue by shared_ptr
 *     explicit Holder(const std::shared_ptr<ProducerConsumerQueue>&);
 *
 *     // Convenience for getting down to our PCQ
 *     const Closer& operator->() const;
 * }
 */

/**
 * multi holder holds by shared_ptr
 */
template <typename PCQ, void (PCQ::*close)()>
class Holder<Multi, PCQ, close> {
    using T = Closer<PCQ, close>;

public:
    template <typename U>
    explicit Holder(const std::shared_ptr<U>& u) : _data(std::make_shared<T>(u)) {}

    const T& operator->() const {
        return *_data;
    }

private:
    std::shared_ptr<T> _data;
};

/**
 * single holder holds the closer directly, but still allows operator->() access
 */
template <typename PCQ, void (PCQ::*close)()>
class Holder<Single, PCQ, close> {
    using T = Closer<PCQ, close>;

public:
    template <typename U>
    explicit Holder(const std::shared_ptr<U>& u) : _data(u) {}

    const T& operator->() const {
        return _data;
    }

private:
    T _data;
};

/**
 * Consumer state holds the state needed to manage the consumers of the queue.  It either invariants
 * on multiple waiting consumers (if not multi) or queues them unfairly under the same condvar (for
 * multi).
 */
template <ConsumerKind isMulti>
class ConsumerState {
public:
    // condition variable to block on for waiting consumers
    stdx::condition_variable& cv() {
        return _cv;
    }

    // operator size_t tells how many consumers there are
    operator size_t() const {
        return _data;
    }

    // Waiter type for blocking consumers
    class Waiter {
    public:
        explicit Waiter(ConsumerState& x) : _x(x._data) {
            invariant(isMulti || !_x);
            ++_x;
        }

        Waiter(const Waiter&) = delete;
        Waiter& operator=(const Waiter&) = delete;
        Waiter(Waiter&&) = delete;
        Waiter& operator=(Waiter&&) = delete;

        ~Waiter() {
            --_x;
        }

    private:
        size_t& _x;
    };

private:
    size_t _data = 0;
    stdx::condition_variable _cv;
};

/**
 * The producer state holds the state needed to manage producers waiting on the queue.  In its multi
 * version, it FIFO queues producer requests.  In its single version, it invariants on multiple
 * producers.
 */
template <ProducerKind, typename Options>
class ProducerState;
/**
 * {
 *     // we require access to the Options for maxProducerQueueDepth
 *     explicit ProducerState(const Options&);
 *
 *     // The cv consumers should notify after consumption
 *     stdx::condition_variable& cv();
 *
 *     // The amount the oldest producer wants to inject
 *     size_t wants() const;
 *
 *     // The number of producers
 *     operator size_t() const;
 *
 *     // The amount of work queued in all the waiting producers
 *     size_t queueDepth() const;
 *
 *     // A Waiter type for waiting producers
 *     class Waiter {
 *         // queueing a producer modifies the producer state and takes how much the producer wants
 *         // to produce
 *         explicit Waiter(ProducerState& state, size_t wants);
 *         ~Waiter();
 *
 *         // The cv that producer should block on
 *         stdx::condition_variable& cv();
 *
 *         // If this producer is the current top level producer.  The producer thread should block
 *         // on cv() and check isAtFrontOfQueue() before unblocking.
 *         bool isAtFrontOfQueue() const;
 *     };
 * }
 */

/**
 * Single producer state holds one long live condvar for producers and the one producers desired
 * capacity.  It invariants if another producer shows up.
 */
template <typename Options>
class ProducerState<SingleProducer, Options> {
public:
    explicit ProducerState(const Options&) {}

    stdx::condition_variable& cv() {
        return _cv;
    }

    size_t wants() const {
        return _data;
    }

    operator size_t() const {
        return _data ? 1 : 0;
    }

    size_t queueDepth() const {
        return _data;
    }

    class Waiter {
    public:
        explicit Waiter(ProducerState& x, size_t wants) : _x(x) {
            invariant(!_x);
            _x._data = wants;
        }

        Waiter(const Waiter&) = delete;
        Waiter& operator=(const Waiter&) = delete;
        Waiter(Waiter&&) = delete;
        Waiter& operator=(Waiter&&) = delete;

        ~Waiter() {
            _x._data = 0;
        }

        stdx::condition_variable& cv() {
            return _x._cv;
        }

        bool isAtFrontOfQueue() const {
            return true;
        }

    public:
        ProducerState& _x;
    };

private:
    size_t _data = 0;

    stdx::condition_variable _cv;
};

/**
 * The multi-producer holds a linked list of producers, along with cvs to wake them and their
 * desired capacity.
 */
template <typename Options>
class ProducerState<MultiProducer, Options> {
private:
    struct ProducerWants;

public:
    explicit ProducerState(const Options& options)
        : _maxProducerQueueDepth(options.maxProducerQueueDepth) {}

    stdx::condition_variable& cv() {
        return _data.front().cv;
    }

    size_t wants() const {
        return _data.front().wants;
    }

    operator size_t() const {
        return _data.size();
    }

    size_t queueDepth() const {
        return _producerQueueDepth;
    }

    class Waiter {
    public:
        explicit Waiter(ProducerState& x, size_t wants) : _x(x) {
            uassert(ErrorCodes::ProducerConsumerQueueProducerQueueDepthExceeded,
                    str::stream() << "ProducerConsumerQueue producer queue depth exceeded, "
                                  << (_x._producerQueueDepth + wants)
                                  << " > "
                                  << _x._maxProducerQueueDepth,
                    _x._maxProducerQueueDepth == std::numeric_limits<size_t>::max() ||
                        _x._producerQueueDepth + wants <= _x._maxProducerQueueDepth);

            _x._producerQueueDepth += wants;
            _x._data.emplace_back(wants);
            _iter = std::prev(_x._data.end());
        }

        Waiter(const Waiter&) = delete;
        Waiter& operator=(const Waiter&) = delete;
        Waiter(Waiter&&) = delete;
        Waiter& operator=(Waiter&&) = delete;

        ~Waiter() {
            _x._producerQueueDepth -= _iter->wants;
            _x._data.erase(_iter);
        }

        stdx::condition_variable& cv() {
            return _iter->cv;
        }

        bool isAtFrontOfQueue() const {
            return _x._data.begin() == _iter;
        }

    private:
        // We store these in a linked list to allow for removal from the middle of the queue if
        // we're interrupted
        ProducerState& _x;
        typename std::list<ProducerWants>::iterator _iter;
    };

private:
    // One of these is allocated for each producer that blocks on pushing to the queue
    struct ProducerWants {
        ProducerWants(size_t s) : wants(s) {}

        size_t wants;
        // Each producer has their own cv, so that they can be woken individually in FIFO order
        stdx::condition_variable cv;
    };

    // A list of producers that want to push to the queue
    std::list<ProducerWants> _data;
    size_t _producerQueueDepth = 0;
    const size_t& _maxProducerQueueDepth;
};

template <typename CostFunc>
struct PCQOptions {
    // Maximum queue depth in cost func units
    size_t maxQueueDepth = std::numeric_limits<size_t>::max();

    // Cost function for the queue
    CostFunc costFunc;

    // maximum capacity for all waiting producers measured in cost func units
    size_t maxProducerQueueDepth = std::numeric_limits<size_t>::max();
};

/**
 * A bounded, blocking, interruptible, thread safe, cost parametrizable, X-producer, X-consumer
 * queue.
 *
 * Properties:
 *   bounded - the queue can be limited in the number of items it can hold
 *   blocking - when the queue is full, or has no entries, callers block
 *   thread safe - the queue can be accessed safely from multiple threads at the same time
 *   cost parametrizable - the cost of items in the queue need not be equal. I.e. your items could
 *                          be discrete byte buffers and the queue depth measured in bytes, so that
 *                          the queue could hold one large buffer, or many smaller ones
 *   X-producer - 1 or many threads may push work into the queue.  For multi-producer, producers
 *                produce in FIFO order.
 *   X-consumer - 1 or many threads may pop work out of the queue
 *   interruptible - All of the blocking methods on this type take an interruptible.
 *
 * Exceptions outside the interruptible include:
 *   closure of queue endpoints that isn't a pop() after producer end closed
 *     ErrorCodes::ProducerConsumerQueueEndClosed
 *   pushes with batches that exceed the max queue size
 *     ErrorCodes::ProducerConsumerQueueBatchTooLarge
 *   too many producers blocked on the queue
 *     ErrorCodes::ProducerConsumerQueueProducerQueueDepthExceeded
 *   pop() after producer end closed (I.e. eof)
 *     ErrorCodes::ProducerConsumerQueueConsumed
 *
 * Cost Function:
 *   The cost function must have a call operator which takes a const T& and returns the cost in
 *   size_t units. It must be pure across moves for a given T and never return zero. The intent of
 *   the cost function is to express the kind of bounds the queue provides, rather than to
 *   specialize behavior for a type. I.e. you should not specialize the default cost function and
 *   the cost function should always be explicit in the type.
 */
template <typename T, ProducerKind producerKind, ConsumerKind consumerKind, typename CostFunc>
class ProducerConsumerQueue {
public:
    struct Stats {
        size_t queueDepth;
        size_t waitingConsumers;
        size_t waitingProducers;
        size_t producerQueueDepth;
        // TODO more stats
        //
        // totalTimeBlocked on either side
        // closed ends
        // count of producers and consumers (blocked, or existing if we're a pipe)
    };

    using Options = PCQOptions<CostFunc>;

    // By default the queue depth is unlimited
    explicit ProducerConsumerQueue(Options options = {})
        : _options(std::move(options)), _producers(_options) {}

    ProducerConsumerQueue(const ProducerConsumerQueue&) = delete;
    ProducerConsumerQueue& operator=(const ProducerConsumerQueue&) = delete;

    ProducerConsumerQueue(ProducerConsumerQueue&&) = delete;
    ProducerConsumerQueue& operator=(ProducerConsumerQueue&&) = delete;

    ~ProducerConsumerQueue() {
        invariant(!_producers);
        invariant(!_consumers);
    }

    // Pushes the passed T into the queue
    //
    // Leaves T unchanged if an interrupt exception is thrown while waiting for space
    void push(T&& t, Interruptible* interruptible = Interruptible::notInterruptible()) {
        _pushRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            auto cost = _invokeCostFunc(t, lk);
            uassert(ErrorCodes::ProducerConsumerQueueBatchTooLarge,
                    str::stream() << "cost of item (" << cost
                                  << ") larger than maximum queue size ("
                                  << _options.maxQueueDepth
                                  << ")",
                    cost <= _options.maxQueueDepth);

            _waitForSpace(lk, cost, interruptible);
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
    template <typename StartIterator, typename EndIterator>
    void pushMany(StartIterator start,
                  EndIterator last,
                  Interruptible* interruptible = Interruptible::notInterruptible()) {
        return _pushRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            size_t cost = 0;
            for (auto iter = start; iter != last; ++iter) {
                cost += _invokeCostFunc(*iter, lk);
            }

            uassert(ErrorCodes::ProducerConsumerQueueBatchTooLarge,
                    str::stream() << "cost of items in batch (" << cost
                                  << ") larger than maximum queue size ("
                                  << _options.maxQueueDepth
                                  << ")",
                    cost <= _options.maxQueueDepth);

            _waitForSpace(lk, cost, interruptible);

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
    T pop(Interruptible* interruptible = Interruptible::notInterruptible()) {
        return _popRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            _waitForNonEmpty(lk, interruptible);
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
    template <typename OutputIterator>
    std::pair<size_t, OutputIterator> popMany(
        OutputIterator iterator, Interruptible* interruptible = Interruptible::notInterruptible()) {
        return popManyUpTo(_options.maxQueueDepth, iterator, interruptible);
    }

    // Waits for at least one item in the queue, then pops items out of the queue until it would
    // block, or we've exceeded our budget
    //
    // OutputIterator must not throw on move assignment to *iter or popped values may be lost
    // TODO: add sfinae to check to enforce
    //
    // Returns the cost value of the items extracted, along with the updated output iterator
    template <typename OutputIterator>
    std::pair<size_t, OutputIterator> popManyUpTo(
        size_t budget,
        OutputIterator iterator,
        Interruptible* interruptible = Interruptible::notInterruptible()) {
        return _popRunner([&](stdx::unique_lock<stdx::mutex>& lk) {
            size_t cost = 0;

            _waitForNonEmpty(lk, interruptible);

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

    Stats getStats() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        Stats stats;
        stats.queueDepth = _current;
        stats.waitingConsumers = _consumers;
        stats.waitingProducers = _producers;
        stats.producerQueueDepth = _producers.queueDepth();
        return stats;
    }

    class Pipe;

    /**
     * This type wraps up the Producer portion of the PCQ api.  See Pipe for more details.
     */
    class Producer {
    public:
        Producer() = default;

        void push(T&& t, Interruptible* interruptible = Interruptible::notInterruptible()) const {
            _parent->push(std::move(t), interruptible);
        }

        template <typename StartIterator, typename EndIterator>
        void pushMany(StartIterator&& start,
                      EndIterator&& last,
                      Interruptible* interruptible = Interruptible::notInterruptible()) const {
            _parent->pushMany(
                std::forward<StartIterator>(start), std::forward<EndIterator>(last), interruptible);
        }

        bool tryPush(T&& t) const {
            return _parent->tryPush(std::move(t));
        }

        // Note that calling close() here is different than just allowing your pipe end to expire.
        // This close() will close the end for all producers (possibly causing other's to fail)
        // rather than closing after all producers have gone away.
        void close() const {
            _parent->closeProducerEnd();
        }

    private:
        friend class ProducerConsumerQueue::Pipe;

        explicit Producer(const std::shared_ptr<ProducerConsumerQueue>& parent) : _parent(parent) {}

        Holder<producerKind, ProducerConsumerQueue, &ProducerConsumerQueue::closeProducerEnd>
            _parent;
    };

    /**
     * This type wraps up the Consumer portion of the PCQ api.  See Pipe for more details.
     */
    class Consumer {
    public:
        Consumer() = default;

        T pop(Interruptible* interruptible = Interruptible::notInterruptible()) const {
            return _parent->pop(interruptible);
        }

        template <typename OutputIterator>
        std::pair<size_t, OutputIterator> popMany(
            OutputIterator&& iterator,
            Interruptible* interruptible = Interruptible::notInterruptible()) const {
            return _parent->popMany(std::forward<OutputIterator>(iterator), interruptible);
        }

        template <typename OutputIterator>
        std::pair<size_t, OutputIterator> popManyUpTo(
            size_t budget,
            OutputIterator&& iterator,
            Interruptible* interruptible = Interruptible::notInterruptible()) const {
            return _parent->popManyUpTo(
                budget, std::forward<OutputIterator>(iterator), interruptible);
        }

        boost::optional<T> tryPop() const {
            return _parent->tryPop();
        }

        // Note that calling close() here is different than just allowing your pipe end to expire.
        // This close() will close the end for all consumers (possibly causing other's to fail)
        // rather than closing after all consumers have gone away.
        void close() const {
            _parent->closeConsumerEnd();
        }

    private:
        friend class ProducerConsumerQueue::Pipe;

        explicit Consumer(const std::shared_ptr<ProducerConsumerQueue>& parent) : _parent(parent) {}

        Holder<consumerKind, ProducerConsumerQueue, &ProducerConsumerQueue::closeConsumerEnd>
            _parent;
    };

    /**
     * This type wraps up the Controller portion of the PCQ api.  See Pipe for more details.
     */
    class Controller {
    public:
        Controller() = default;

        Stats getStats() const {
            return _parent->getStats();
        }

    private:
        friend class ProducerConsumerQueue::Pipe;

        explicit Controller(const std::shared_ptr<ProducerConsumerQueue>& parent)
            : _parent(parent) {}

        std::shared_ptr<ProducerConsumerQueue> _parent;
    };

    /**
     * This Pipe type offers a safe way of distributing portions of the ProducerConsumerQueue object
     * via 3 distinct interfaces.  It takes into account whether the PCQ is single/multi
     * producer/consumer in making the pipe ends copyable, or merely moveable, and closes that end
     * of the pipe when all owners go away.  In this way, it enforces the contract of the PCQ and
     * offers easy coordination of work.
     *
     * The administrative api is reflected in the "Controller member"
     */
    class Pipe {
    public:
        explicit Pipe(typename ProducerConsumerQueue::Options options = {})
            : Pipe(std::make_shared<ProducerConsumerQueue>(std::move(options))) {}

        Producer producer;
        Controller controller;
        Consumer consumer;

    private:
        // This constructor is private, because the logic around closing endpoints when all producer
        // or consumer references go away makes it difficult to use if there are public parent pcq
        // references floating around.
        explicit Pipe(const std::shared_ptr<ProducerConsumerQueue>& parent)
            : producer(parent), controller(parent), consumer(parent) {}
    };

private:
    using Consumers = ConsumerState<consumerKind>;
    using Producers = ProducerState<producerKind, Options>;

    size_t _invokeCostFunc(const T& t, WithLock) {
        auto cost = _options.costFunc(t);
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
        uassert(ErrorCodes::ProducerConsumerQueueConsumed,
                "Producer end closed and values exhausted",
                !(_producerEndClosed && _queue.empty()));
    }

    void _notifyIfNecessary(WithLock) {
        // If we've closed the consumer end, or if the production end is closed and we've exhausted
        // the queue, wake everyone up and get out of here
        if (_consumerEndClosed || (_queue.empty() && _producerEndClosed)) {
            // Whether this one or many consumers, they all listen on the same cv
            if (_consumers) {
                _consumers.cv().notify_all();
            }

            // In multi-producer situations, the producers notify each other in turn
            if (_producers) {
                _producers.cv().notify_one();
            }

            return;
        }

        // If a producer is queued, and we have enough space for it to push its work
        if (_producers && _current + _producers.wants() <= _options.maxQueueDepth) {
            _producers.cv().notify_one();

            return;
        }

        // If we have consumers and anything in the queue, notify consumers
        if (_consumers && _queue.size()) {
            _consumers.cv().notify_one();

            return;
        }
    }

    template <typename Callback>
    auto _pushRunner(Callback&& cb) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        _checkProducerClosed(lk);

        const auto guard = makeGuard([&] { _notifyIfNecessary(lk); });

        return cb(lk);
    }

    template <typename Callback>
    auto _popRunner(Callback&& cb) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        _checkConsumerClosed(lk);

        const auto guard = makeGuard([&] { _notifyIfNecessary(lk); });

        return cb(lk);
    }

    bool _tryPush(WithLock wl, T&& t) {
        size_t cost = _invokeCostFunc(t, wl);
        if (_current + cost <= _options.maxQueueDepth) {
            _queue.emplace(std::move(t));
            _current += cost;
            return true;
        }

        return false;
    }

    void _push(WithLock wl, T&& t) {
        size_t cost = _invokeCostFunc(t, wl);
        invariant(_current + cost <= _options.maxQueueDepth);

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

    void _waitForSpace(stdx::unique_lock<stdx::mutex>& lk,
                       size_t cost,
                       Interruptible* interruptible) {
        // We do some pre-flight checks to avoid creating a cv if we don't need one
        _checkProducerClosed(lk);

        if (!_producers && _current + cost <= _options.maxQueueDepth) {
            return;
        }

        typename Producers::Waiter waiter(_producers, cost);

        interruptible->waitForConditionOrInterrupt(waiter.cv(), lk, [&] {
            _checkProducerClosed(lk);

            return waiter.isAtFrontOfQueue() && _current + cost <= _options.maxQueueDepth;
        });
    }

    void _waitForNonEmpty(stdx::unique_lock<stdx::mutex>& lk, Interruptible* interruptible) {
        typename Consumers::Waiter waiter(_consumers);

        interruptible->waitForConditionOrInterrupt(_consumers.cv(), lk, [&] {
            _checkConsumerClosed(lk);
            return _queue.size();
        });
    }

    mutable stdx::mutex _mutex;

    Options _options;

    // Current size of the queue
    size_t _current = 0;

    std::queue<T> _queue;

    // State for waiting consumers and producers
    Consumers _consumers;
    Producers _producers;

    // Flags that we're shutting down the queue
    bool _consumerEndClosed = false;
    bool _producerEndClosed = false;
};

}  // namespace producer_consumer_queue_detail

template <typename T, typename CostFunc = producer_consumer_queue_detail::DefaultCostFunction>
using MultiProducerMultiConsumerQueue = producer_consumer_queue_detail::ProducerConsumerQueue<
    T,
    producer_consumer_queue_detail::MultiProducer,
    producer_consumer_queue_detail::MultiConsumer,
    CostFunc>;

template <typename T, typename CostFunc = producer_consumer_queue_detail::DefaultCostFunction>
using MultiProducerSingleConsumerQueue = producer_consumer_queue_detail::ProducerConsumerQueue<
    T,
    producer_consumer_queue_detail::MultiProducer,
    producer_consumer_queue_detail::SingleConsumer,
    CostFunc>;

template <typename T, typename CostFunc = producer_consumer_queue_detail::DefaultCostFunction>
using SingleProducerMultiConsumerQueue = producer_consumer_queue_detail::ProducerConsumerQueue<
    T,
    producer_consumer_queue_detail::SingleProducer,
    producer_consumer_queue_detail::MultiConsumer,
    CostFunc>;

template <typename T, typename CostFunc = producer_consumer_queue_detail::DefaultCostFunction>
using SingleProducerSingleConsumerQueue = producer_consumer_queue_detail::ProducerConsumerQueue<
    T,
    producer_consumer_queue_detail::SingleProducer,
    producer_consumer_queue_detail::SingleConsumer,
    CostFunc>;

}  // namespace mongo
