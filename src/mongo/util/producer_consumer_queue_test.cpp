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

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include "mongo/util/producer_consumer_queue.h"

#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

using namespace producer_consumer_queue_detail;

template <bool isMultiProducer, bool isMultiConsumer, typename... Args>
class ProducerConsumerQueueTestHelper;

template <bool isMultiProducer, bool isMultiConsumer>
class ProducerConsumerQueueTestHelper<isMultiProducer, isMultiConsumer, OperationContext> {
public:
    ProducerConsumerQueueTestHelper(ServiceContext* serviceCtx) : _serviceCtx(serviceCtx) {}

    template <typename Callback>
    stdx::thread runThread(StringData name, Callback&& cb) {
        return stdx::thread([this, name, cb] {
            auto client = _serviceCtx->makeClient(name.toString());
            auto opCtx = client->makeOperationContext();

            cb(opCtx.get());
        });
    }

    template <typename T, typename CostFunc = DefaultCostFunction>
    using ProducerConsumerQueue =
        ProducerConsumerQueue<T,
                              isMultiProducer ? MultiProducer : SingleProducer,
                              isMultiConsumer ? MultiConsumer : SingleConsumer,
                              CostFunc>;

private:
    ServiceContext* _serviceCtx;
};

template <bool isMultiProducer, bool isMultiConsumer, typename Timeout>
class ProducerConsumerQueueTestHelper<isMultiProducer, isMultiConsumer, OperationContext, Timeout> {
public:
    ProducerConsumerQueueTestHelper(ServiceContext* serviceCtx, Timeout timeout)
        : _serviceCtx(serviceCtx), _timeout(timeout) {}

    template <typename Callback>
    stdx::thread runThread(StringData name, Callback&& cb) {
        return stdx::thread([this, name, cb] {
            auto client = _serviceCtx->makeClient(name.toString());
            auto opCtx = client->makeOperationContext();

            opCtx->runWithDeadline(
                _timeout, ErrorCodes::ExceededTimeLimit, [&] { cb(opCtx.get()); });
        });
    }

    template <typename T, typename CostFunc = DefaultCostFunction>
    using ProducerConsumerQueue =
        ProducerConsumerQueue<T,
                              isMultiProducer ? MultiProducer : SingleProducer,
                              isMultiConsumer ? MultiConsumer : SingleConsumer,
                              CostFunc>;

private:
    ServiceContext* _serviceCtx;
    Timeout _timeout;
};

template <bool requiresMultiProducer, bool requiresMultiConsumer, typename Callback>
std::enable_if_t<!requiresMultiProducer && !requiresMultiConsumer> runCallbackWithPerms(
    Callback&& cb) {
    std::forward<Callback>(cb)(std::true_type{}, std::true_type{});
    std::forward<Callback>(cb)(std::true_type{}, std::false_type{});
    std::forward<Callback>(cb)(std::false_type{}, std::true_type{});
    std::forward<Callback>(cb)(std::false_type{}, std::false_type{});
}

template <bool requiresMultiProducer, bool requiresMultiConsumer, typename Callback>
std::enable_if_t<requiresMultiProducer && !requiresMultiConsumer> runCallbackWithPerms(
    Callback&& cb) {
    std::forward<Callback>(cb)(std::true_type{}, std::true_type{});
    std::forward<Callback>(cb)(std::true_type{}, std::false_type{});
}

template <bool requiresMultiProducer, bool requiresMultiConsumer, typename Callback>
std::enable_if_t<!requiresMultiProducer && requiresMultiConsumer> runCallbackWithPerms(
    Callback&& cb) {
    std::forward<Callback>(cb)(std::true_type{}, std::true_type{});
    std::forward<Callback>(cb)(std::false_type{}, std::true_type{});
}

template <bool requiresMultiProducer, bool requiresMultiConsumer, typename Callback>
std::enable_if_t<requiresMultiProducer && requiresMultiConsumer> runCallbackWithPerms(
    Callback&& cb) {
    std::forward<Callback>(cb)(std::true_type{}, std::true_type{});
}

class ProducerConsumerQueueTest : public unittest::Test {
public:
    template <bool requiresMultiProducer, bool requiresMultiConsumer, typename Callback>
    void runPermutations(Callback&& callback) {
        runCallbackWithPerms<requiresMultiProducer, requiresMultiConsumer>([&](auto x_, auto y_) {
            constexpr bool x = decltype(x_)::value;
            constexpr bool y = decltype(y_)::value;
            callback(ProducerConsumerQueueTestHelper<x, y, OperationContext>(_serviceCtx.get()));
        });
    }

    template <bool requiresMultiProducer, bool requiresMultiConsumer, typename Callback>
    void runTimeoutPermutations(Callback&& callback) {
        const Milliseconds duration(10);

        runCallbackWithPerms<requiresMultiProducer, requiresMultiConsumer>([&](auto x_, auto y_) {
            constexpr bool x = decltype(x_)::value;
            constexpr bool y = decltype(y_)::value;
            callback(ProducerConsumerQueueTestHelper<x, y, OperationContext, Date_t>(
                _serviceCtx.get(), _serviceCtx->getPreciseClockSource()->now() + duration));
        });
    }

private:
    ServiceContext::UniqueServiceContext _serviceCtx = ServiceContext::make();
};

class MoveOnly {
public:
    struct CostFunc {
        CostFunc() = default;
        explicit CostFunc(size_t val) : val(val) {}

        size_t operator()(const MoveOnly& mo) const {
            return val + *mo._val;
        }

        size_t val = 0;
    };

    explicit MoveOnly(int i) : _val(i) {}

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;

    MoveOnly(MoveOnly&& other) : _val(other._val) {
        other._val.reset();
    }

    MoveOnly& operator=(MoveOnly&& other) {
        if (&other == this) {
            return *this;
        }

        _val = other._val;
        other._val.reset();

        return *this;
    }

    bool movedFrom() const {
        return !_val;
    }

    friend bool operator==(const MoveOnly& lhs, const MoveOnly& rhs) {
        return *lhs._val == *rhs._val;
    }

    friend bool operator!=(const MoveOnly& lhs, const MoveOnly& rhs) {
        return !(lhs == rhs);
    }

    friend std::ostream& operator<<(std::ostream& os, const MoveOnly& mo) {
        return (os << "MoveOnly(" << *mo._val << ")");
    }

private:
    boost::optional<int> _val;
};

#define PRODUCER_CONSUMER_QUEUE_TEST(name, ...) \
    struct name##CB {                           \
        template <typename Helper>              \
        void operator()(Helper&& helper);       \
    };                                          \
    TEST_F(ProducerConsumerQueueTest, name) {   \
        __VA_ARGS__(name##CB{});                \
    }                                           \
    template <typename Helper>                  \
    void name##CB::operator()(Helper&& helper)

PRODUCER_CONSUMER_QUEUE_TEST(basicPushPop, runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    helper.runThread("Producer", [&](OperationContext* opCtx) { pcq.push(MoveOnly(1), opCtx); })
        .join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 1ul);

    helper
        .runThread("Consumer",
                   [&](OperationContext* opCtx) { ASSERT_EQUALS(pcq.pop(opCtx), MoveOnly(1)); })
        .join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(closeConsumerEnd, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    PCQ pcq(options);

    pcq.push(MoveOnly(1));

    auto producer = helper.runThread("Producer", [&](OperationContext* opCtx) {
        ASSERT_THROWS_CODE(
            pcq.push(MoveOnly(2), opCtx), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);
    });

    ASSERT_EQUALS(pcq.getStats().queueDepth, 1ul);

    pcq.closeConsumerEnd();

    ASSERT_THROWS_CODE(pcq.pop(), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);

    producer.join();
}

PRODUCER_CONSUMER_QUEUE_TEST(closeProducerEndImmediate, runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    pcq.push(MoveOnly(1));
    pcq.closeProducerEnd();

    helper
        .runThread("Consumer",
                   [&](OperationContext* opCtx) {
                       ASSERT_EQUALS(pcq.pop(opCtx), MoveOnly(1));

                       ASSERT_THROWS_CODE(
                           pcq.pop(opCtx), DBException, ErrorCodes::ProducerConsumerQueueConsumed);
                   })
        .join();
}

PRODUCER_CONSUMER_QUEUE_TEST(closeProducerEndBlocking, runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    auto consumer = helper.runThread("Consumer", [&](OperationContext* opCtx) {
        ASSERT_THROWS_CODE(pcq.pop(opCtx), DBException, ErrorCodes::ProducerConsumerQueueConsumed);
    });

    pcq.closeProducerEnd();

    consumer.join();
}

PRODUCER_CONSUMER_QUEUE_TEST(popsWithTimeout, runTimeoutPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    helper
        .runThread(
            "Consumer",
            [&](OperationContext* opCtx) {
                ASSERT_THROWS_CODE(pcq.pop(opCtx), DBException, ErrorCodes::ExceededTimeLimit);

                ASSERT_THROWS_CODE(pcq.popMany(opCtx), DBException, ErrorCodes::ExceededTimeLimit);

                ASSERT_THROWS_CODE(
                    pcq.popManyUpTo(1000, opCtx), DBException, ErrorCodes::ExceededTimeLimit);
            })
        .join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(pushesWithTimeout, runTimeoutPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    PCQ pcq(options);

    {
        MoveOnly mo(1);
        pcq.push(std::move(mo));
        ASSERT(mo.movedFrom());
    }

    helper
        .runThread("Consumer",
                   [&](OperationContext* opCtx) {
                       {
                           MoveOnly mo(2);
                           ASSERT_THROWS_CODE(pcq.push(std::move(mo), opCtx),
                                              DBException,
                                              ErrorCodes::ExceededTimeLimit);
                           ASSERT_EQUALS(pcq.getStats().queueDepth, 1ul);
                           ASSERT(!mo.movedFrom());
                           ASSERT_EQUALS(mo, MoveOnly(2));
                       }

                       {
                           std::vector<MoveOnly> vec;
                           vec.emplace_back(MoveOnly(2));

                           auto iter = begin(vec);
                           ASSERT_THROWS_CODE(pcq.pushMany(iter, end(vec), opCtx),
                                              DBException,
                                              ErrorCodes::ExceededTimeLimit);
                           ASSERT_EQUALS(pcq.getStats().queueDepth, 1ul);
                           ASSERT(!vec[0].movedFrom());
                           ASSERT_EQUALS(vec[0], MoveOnly(2));
                       }
                   })
        .join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 1ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(basicPushPopWithBlocking, runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    auto consumer = helper.runThread(
        "Consumer", [&](OperationContext* opCtx) { ASSERT_EQUALS(pcq.pop(opCtx), MoveOnly(1)); });

    auto producer = helper.runThread(
        "Producer", [&](OperationContext* opCtx) { pcq.push(MoveOnly(1), opCtx); });

    consumer.join();
    producer.join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(multipleStepPushPopWithBlocking, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    PCQ pcq(options);

    auto consumer = helper.runThread("Consumer", [&](OperationContext* opCtx) {
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(pcq.pop(opCtx), MoveOnly(i));
        }
    });

    auto producer = helper.runThread("Producer", [&](OperationContext* opCtx) {
        for (int i = 0; i < 10; ++i) {
            pcq.push(MoveOnly(i), opCtx);
        }
    });

    consumer.join();
    producer.join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(pushTooLarge, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc>;
    {
        typename PCQ::Options options;
        options.maxQueueDepth = 1;
        PCQ pcq(options);

        helper
            .runThread("Producer",
                       [&](OperationContext* opCtx) {
                           ASSERT_THROWS_CODE(pcq.push(MoveOnly(2), opCtx),
                                              DBException,
                                              ErrorCodes::ProducerConsumerQueueBatchTooLarge);
                       })
            .join();
    }

    {
        typename PCQ::Options options;
        options.maxQueueDepth = 4;
        PCQ pcq(options);

        std::vector<MoveOnly> vec;
        vec.push_back(MoveOnly(3));
        vec.push_back(MoveOnly(3));

        helper
            .runThread("Producer",
                       [&](OperationContext* opCtx) {
                           ASSERT_THROWS_CODE(pcq.pushMany(begin(vec), end(vec), opCtx),
                                              DBException,
                                              ErrorCodes::ProducerConsumerQueueBatchTooLarge);
                       })
            .join();
    }
}

PRODUCER_CONSUMER_QUEUE_TEST(pushWouldOverPush, runPermutations<true, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    options.maxProducerQueueDepth = 2;
    PCQ pcq(options);

    pcq.push(MoveOnly(1));

    auto threadA = helper.runThread("ProducerA",
                                    [&](OperationContext* opCtx) { pcq.push(MoveOnly(1), opCtx); });

    while (pcq.getStats().waitingProducers < 1) {
        stdx::this_thread::yield();
    }

    auto threadB = helper.runThread("ProducerB",
                                    [&](OperationContext* opCtx) { pcq.push(MoveOnly(1), opCtx); });

    while (pcq.getStats().waitingProducers < 2) {
        stdx::this_thread::yield();
    }

    helper
        .runThread("ProducerC",
                   [&](OperationContext* opCtx) {
                       ASSERT_THROWS_CODE(
                           pcq.push(MoveOnly(1), opCtx),
                           DBException,
                           ErrorCodes::ProducerConsumerQueueProducerQueueDepthExceeded);
                   })
        .join();

    pcq.pop();
    threadA.join();

    pcq.pop();
    threadB.join();
}

PRODUCER_CONSUMER_QUEUE_TEST(pushManyPopWithoutBlocking, runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    helper
        .runThread("Producer",
                   [&](OperationContext* opCtx) {
                       std::vector<MoveOnly> vec;
                       for (int i = 0; i < 10; ++i) {
                           vec.emplace_back(MoveOnly(i));
                       }

                       pcq.pushMany(begin(vec), end(vec), opCtx);
                   })
        .join();

    helper
        .runThread("Consumer",
                   [&](OperationContext* opCtx) {
                       for (int i = 0; i < 10; ++i) {
                           ASSERT_EQUALS(pcq.pop(opCtx), MoveOnly(i));
                       }
                   })
        .join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(popManyPopWithBlocking, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 2;
    PCQ pcq(options);

    auto consumer = helper.runThread("Consumer", [&](OperationContext* opCtx) {
        for (int i = 0; i < 10; i = i + 2) {
            std::deque<MoveOnly> out;
            std::tie(out, std::ignore) = pcq.popMany(opCtx);

            ASSERT_EQUALS(out.size(), 2ul);
            ASSERT_EQUALS(out[0], MoveOnly(i));
            ASSERT_EQUALS(out[1], MoveOnly(i + 1));
        }
    });

    auto producer = helper.runThread("Producer", [&](OperationContext* opCtx) {
        std::vector<MoveOnly> vec;
        for (int i = 0; i < 10; ++i) {
            vec.emplace_back(MoveOnly(i));
        }

        for (auto iter = begin(vec); iter != end(vec); iter += 2) {
            pcq.pushMany(iter, iter + 2);
        }
    });

    consumer.join();
    producer.join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(popManyUpToPopWithBlocking, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 4;
    PCQ pcq(options);

    auto consumer = helper.runThread("Consumer", [&](OperationContext* opCtx) {
        for (int i = 0; i < 10; i = i + 2) {
            std::deque<MoveOnly> out;
            size_t spent;

            std::tie(out, spent) = pcq.popManyUpTo(2, opCtx);

            ASSERT_EQUALS(spent, 2ul);
            ASSERT_EQUALS(out.size(), 2ul);
            ASSERT_EQUALS(out[0], MoveOnly(i));
            ASSERT_EQUALS(out[1], MoveOnly(i + 1));
        }
    });

    auto producer = helper.runThread("Producer", [&](OperationContext* opCtx) {
        std::vector<MoveOnly> vec;
        for (int i = 0; i < 10; ++i) {
            vec.emplace_back(MoveOnly(i));
        }

        for (auto iter = begin(vec); iter != end(vec); iter += 2) {
            pcq.pushMany(iter, iter + 2);
        }
    });

    consumer.join();
    producer.join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(popManyUpToPopWithBlockingWithSpecialCost,
                             runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{};

    auto consumer = helper.runThread("Consumer", [&](OperationContext* opCtx) {
        {
            std::deque<MoveOnly> out;
            size_t spent;
            std::tie(out, spent) = pcq.popManyUpTo(5, opCtx);

            ASSERT_EQUALS(spent, 3ul);
            ASSERT_EQUALS(out.size(), 2ul);
            ASSERT_EQUALS(out[0], MoveOnly(1));
            ASSERT_EQUALS(out[1], MoveOnly(2));
        }

        {
            std::deque<MoveOnly> out;
            size_t spent;
            std::tie(out, spent) = pcq.popManyUpTo(15, opCtx);

            ASSERT_EQUALS(spent, 12ul);
            ASSERT_EQUALS(out.size(), 3ul);
            ASSERT_EQUALS(out[0], MoveOnly(3));
            ASSERT_EQUALS(out[1], MoveOnly(4));
            ASSERT_EQUALS(out[2], MoveOnly(5));
        }

        {
            std::deque<MoveOnly> out;
            size_t spent;
            std::tie(out, spent) = pcq.popManyUpTo(5, opCtx);

            ASSERT_EQUALS(spent, 0ul);
            ASSERT_EQUALS(out.size(), 0ul);
        }
    });

    auto producer = helper.runThread("Producer", [&](OperationContext* opCtx) {
        std::vector<MoveOnly> vec;
        for (int i = 1; i <= 6; ++i) {
            vec.emplace_back(MoveOnly(i));
        }

        pcq.pushMany(begin(vec), end(vec), opCtx);
    });

    consumer.join();
    producer.join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 6ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(singleProducerMultiConsumer, runPermutations<false, true>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    stdx::mutex mutex;
    size_t successes = 0;
    size_t failures = 0;

    std::array<stdx::thread, 3> threads;
    for (auto& thread : threads) {
        thread = helper.runThread("Consumer", [&](OperationContext* opCtx) {
            {
                try {
                    pcq.pop(opCtx);
                    stdx::lock_guard<stdx::mutex> lk(mutex);
                    successes++;
                } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueConsumed>&) {
                    stdx::lock_guard<stdx::mutex> lk(mutex);
                    failures++;
                }
            }
        });
    }

    pcq.push(MoveOnly(1));
    pcq.push(MoveOnly(2));

    pcq.closeProducerEnd();

    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQUALS(successes, 2ul);
    ASSERT_EQUALS(failures, 1ul);

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(multiProducerSingleConsumer, runPermutations<true, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    PCQ pcq(options);

    pcq.push(MoveOnly(1));

    stdx::mutex mutex;
    size_t success = 0;
    size_t failure = 0;

    std::array<stdx::thread, 3> threads;
    for (auto& thread : threads) {
        thread = helper.runThread("Producer", [&](OperationContext* opCtx) {
            {
                try {
                    pcq.push(MoveOnly(1), opCtx);
                    stdx::lock_guard<stdx::mutex> lk(mutex);
                    success++;
                } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>&) {
                    stdx::lock_guard<stdx::mutex> lk(mutex);
                    failure++;
                }
            }
        });
    }

    pcq.pop();

    while (true) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        if (success == 1)
            break;
        stdx::this_thread::yield();
    }
    pcq.closeConsumerEnd();

    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQUALS(success, 1ul);
    ASSERT_EQUALS(failure, 2ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(multiProducersDontLineSkip, runPermutations<true, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc>;
    typename PCQ::Options options;
    options.maxQueueDepth = 2;
    PCQ pcq(options);

    pcq.push(MoveOnly(1));

    auto bigProducer = helper.runThread(
        "ProducerBig", [&](OperationContext* opCtx) { pcq.push(MoveOnly(2), opCtx); });

    while (pcq.getStats().waitingProducers < 1ul) {
        stdx::this_thread::yield();
    }

    auto smallProducer = helper.runThread(
        "ProducerSmall", [&](OperationContext* opCtx) { pcq.push(MoveOnly(1), opCtx); });

    while (pcq.getStats().waitingProducers < 2ul) {
        stdx::this_thread::yield();
    }

    ASSERT_EQUALS(pcq.getStats().waitingProducers, 2ul);

    pcq.pop();
    bigProducer.join();

    ASSERT_EQUALS(pcq.getStats().waitingProducers, 1ul);
    pcq.pop();
    smallProducer.join();
    ASSERT_EQUALS(pcq.getStats().waitingProducers, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(multiProducerMiddleWaiterBreaks, runPermutations<true, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    PCQ pcq(options);

    pcq.push(MoveOnly(1));

    stdx::mutex mutex;
    bool failed = false;
    OperationContext* threadBopCtx = nullptr;

    auto threadA = helper.runThread("ProducerA",
                                    [&](OperationContext* opCtx) { pcq.push(MoveOnly(1), opCtx); });

    while (pcq.getStats().waitingProducers < 1ul) {
        stdx::this_thread::yield();
    };

    auto threadB = helper.runThread("ProducerB", [&](OperationContext* opCtx) {
        {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            threadBopCtx = opCtx;
        }

        try {
            pcq.push(MoveOnly(2), opCtx);
        } catch (const ExceptionFor<ErrorCodes::Interrupted>&) {
            failed = true;
        }
    });

    while (pcq.getStats().waitingProducers < 2ul) {
        stdx::this_thread::yield();
    };

    {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        ASSERT(threadBopCtx != nullptr);
    }

    auto threadC = helper.runThread("ProducerC",
                                    [&](OperationContext* opCtx) { pcq.push(MoveOnly(3), opCtx); });

    while (pcq.getStats().waitingProducers < 3ul) {
        stdx::this_thread::yield();
    };

    {
        stdx::lock_guard<Client> clientLock(*threadBopCtx->getClient());
        threadBopCtx->markKilled(ErrorCodes::Interrupted);
    }

    threadB.join();
    ASSERT(failed);

    ASSERT_EQUALS(pcq.getStats().waitingProducers, 2ul);

    ASSERT_EQUALS(pcq.pop(), MoveOnly(1));
    threadA.join();
    ASSERT_EQUALS(pcq.pop(), MoveOnly(1));
    ASSERT_EQUALS(pcq.pop(), MoveOnly(3));
    threadC.join();

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(pipeCompiles, runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly>::Pipe pipe{};

    // We move from here to make sure that the destructors of the the various closer's actually
    // work correctly
    //
    // At some point this was working with a single move, and this pattern helped me catch some
    // lifetime screw ups
    auto producer = [](auto p) { return std::move(p); }(std::move(pipe.producer));
    auto controller = [](auto c) { return std::move(c); }(std::move(pipe.controller));
    auto consumer = [](auto c) { return std::move(c); }(std::move(pipe.consumer));

    producer.push(MoveOnly(1));
    std::array<MoveOnly, 1> container({MoveOnly(1)});
    producer.pushMany(container.begin(), container.end());
    ASSERT(producer.tryPush(MoveOnly(1)));

    ASSERT_EQUALS(consumer.pop(), MoveOnly(1));
    ASSERT_EQUALS(consumer.popManyUpTo(1ul).second, 1ul);
    ASSERT_EQUALS(consumer.popMany().second, 1ul);
    ASSERT_FALSE(consumer.tryPop());

    producer.close();
    consumer.close();

    ASSERT_THROWS_CODE(consumer.pop(), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);

    ASSERT_THROWS_CODE(
        producer.push(MoveOnly(1)), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);

    auto stats = controller.getStats();
    ASSERT_EQUALS(stats.queueDepth, 0ul);
    ASSERT_EQUALS(stats.waitingConsumers, 0ul);
    ASSERT_EQUALS(stats.waitingProducers, 0ul);
    ASSERT_EQUALS(stats.producerQueueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(pipeProducerEndClosesAfterProducersLeave,
                             runPermutations<true, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    typename PCQ::Pipe pipe(options);

    auto producer = std::move(pipe.producer);
    auto consumer = std::move(pipe.consumer);

    producer.push(MoveOnly(1));

    auto thread2 = helper.runThread(
        "Producer2", [producer](OperationContext* opCtx) { producer.push(MoveOnly(2), opCtx); });

    ASSERT_EQUALS(consumer.pop(), MoveOnly(1));
    thread2.join();

    ASSERT_EQUALS(consumer.pop(), MoveOnly(2));

    auto thread3 =
        helper.runThread("Producer3", [producer = std::move(producer)](OperationContext * opCtx) {
            producer.push(MoveOnly(3), opCtx);
        });

    ASSERT_EQUALS(consumer.pop(), MoveOnly(3));

    thread3.join();

    ASSERT_THROWS_CODE(consumer.pop(), DBException, ErrorCodes::ProducerConsumerQueueConsumed);
}

PRODUCER_CONSUMER_QUEUE_TEST(pipeConsumerEndClosesAfterConsumersLeave,
                             runPermutations<false, true>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly>::Pipe pipe{};
    auto producer = std::move(pipe.producer);
    auto consumer = std::move(pipe.consumer);

    auto thread2 =
        helper.runThread("Consumer2", [consumer](OperationContext* opCtx) { consumer.pop(opCtx); });

    auto thread3 =
        helper.runThread("Consumer3", [consumer = std::move(consumer)](OperationContext * opCtx) {
            consumer.pop(opCtx);
        });

    producer.push(MoveOnly(1));
    producer.push(MoveOnly(1));

    thread2.join();
    thread3.join();

    ASSERT_THROWS_CODE(
        producer.push(MoveOnly(1)), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);
}

PRODUCER_CONSUMER_QUEUE_TEST(basicTryPop, runPermutations<false, false>) {
    typename Helper::template ProducerConsumerQueue<MoveOnly> pcq{};

    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_EQUALS(pcq.getStats().queueDepth, 1ul);

    auto val = pcq.tryPop();

    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val);
    ASSERT_EQUALS(*val, MoveOnly(1));

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(basicTryPush, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly>;
    typename PCQ::Options options;
    options.maxQueueDepth = 1;
    PCQ pcq(options);

    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_FALSE(pcq.tryPush(MoveOnly(2)));

    ASSERT_EQUALS(pcq.getStats().queueDepth, 1ul);

    auto val = pcq.tryPop();
    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val);
    ASSERT_EQUALS(*val, MoveOnly(1));

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(tryPushWithSpecialCost, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc>;
    typename PCQ::Options options;
    options.maxQueueDepth = 5;
    PCQ pcq(options);

    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_TRUE(pcq.tryPush(MoveOnly(2)));
    ASSERT_FALSE(pcq.tryPush(MoveOnly(3)));

    ASSERT_EQUALS(pcq.getStats().queueDepth, 3ul);

    auto val1 = pcq.tryPop();
    ASSERT_EQUALS(pcq.getStats().queueDepth, 2ul);
    auto val2 = pcq.tryPop();
    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val1);
    ASSERT_TRUE(val2);
    ASSERT_EQUALS(*val1, MoveOnly(1));
    ASSERT_EQUALS(*val2, MoveOnly(2));

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

PRODUCER_CONSUMER_QUEUE_TEST(tryPushWithSpecialStatefulCost, runPermutations<false, false>) {
    using PCQ = typename Helper::template ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc>;
    typename PCQ::Options options;
    options.maxQueueDepth = 5;
    options.costFunc = MoveOnly::CostFunc(1);
    PCQ pcq(options);

    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_TRUE(pcq.tryPush(MoveOnly(2)));
    ASSERT_FALSE(pcq.tryPush(MoveOnly(3)));

    ASSERT_EQUALS(pcq.getStats().queueDepth, 5ul);

    auto val1 = pcq.tryPop();
    ASSERT_EQUALS(pcq.getStats().queueDepth, 3ul);
    auto val2 = pcq.tryPop();
    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val1);
    ASSERT_TRUE(val2);
    ASSERT_EQUALS(*val1, MoveOnly(1));
    ASSERT_EQUALS(*val2, MoveOnly(2));

    ASSERT_EQUALS(pcq.getStats().queueDepth, 0ul);
}

}  // namespace

}  // namespace mongo
