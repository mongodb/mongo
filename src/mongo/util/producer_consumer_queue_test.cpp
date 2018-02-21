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

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include "mongo/util/producer_consumer_queue.h"

#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

template <typename... Args>
class ProducerConsumerQueueTestHelper;

template <>
class ProducerConsumerQueueTestHelper<OperationContext> {
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

private:
    ServiceContext* _serviceCtx;
};

template <typename Timeout>
class ProducerConsumerQueueTestHelper<OperationContext, Timeout> {
public:
    ProducerConsumerQueueTestHelper(ServiceContext* serviceCtx, Timeout timeout)
        : _serviceCtx(serviceCtx), _timeout(timeout) {}

    template <typename Callback>
    stdx::thread runThread(StringData name, Callback&& cb) {
        return stdx::thread([this, name, cb] {
            auto client = _serviceCtx->makeClient(name.toString());
            auto opCtx = client->makeOperationContext();

            cb(opCtx.get(), _timeout);
        });
    }

private:
    ServiceContext* _serviceCtx;
    Timeout _timeout;
};

template <>
class ProducerConsumerQueueTestHelper<> {
public:
    ProducerConsumerQueueTestHelper() = default;

    template <typename Callback>
    stdx::thread runThread(StringData name, Callback&& cb) {
        return stdx::thread([this, name, cb] { cb(); });
    }
};

template <typename Timeout>
class ProducerConsumerQueueTestHelper<Timeout> {
public:
    ProducerConsumerQueueTestHelper(Timeout timeout) : _timeout(timeout) {}

    template <typename Callback>
    stdx::thread runThread(StringData name, Callback&& cb) {
        return stdx::thread([this, name, cb] { cb(_timeout); });
    }

private:
    Timeout _timeout;
};

class ProducerConsumerQueueTest : public unittest::Test {
public:
    ProducerConsumerQueueTest() : _serviceCtx(stdx::make_unique<ServiceContextNoop>()) {}

    template <typename Callback>
    stdx::thread runThread(StringData name, Callback&& cb) {
        return stdx::thread([this, name, cb] {
            auto client = _serviceCtx->makeClient(name.toString());
            auto opCtx = client->makeOperationContext();

            cb(opCtx.get());
        });
    }

    template <typename Callback>
    void runPermutations(Callback&& callback) {
        const Minutes duration(30);

        callback(ProducerConsumerQueueTestHelper<OperationContext>(_serviceCtx.get()));
        callback(ProducerConsumerQueueTestHelper<OperationContext, Milliseconds>(_serviceCtx.get(),
                                                                                 duration));
        callback(ProducerConsumerQueueTestHelper<OperationContext, Date_t>(
            _serviceCtx.get(), _serviceCtx->getPreciseClockSource()->now() + duration));
        callback(ProducerConsumerQueueTestHelper<>());
        callback(ProducerConsumerQueueTestHelper<Milliseconds>(duration));
        callback(ProducerConsumerQueueTestHelper<Date_t>(
            _serviceCtx->getPreciseClockSource()->now() + duration));
    }

    template <typename Callback>
    void runTimeoutPermutations(Callback&& callback) {
        const Milliseconds duration(10);

        callback(ProducerConsumerQueueTestHelper<OperationContext, Milliseconds>(_serviceCtx.get(),
                                                                                 duration));
        callback(ProducerConsumerQueueTestHelper<OperationContext, Date_t>(
            _serviceCtx.get(), _serviceCtx->getPreciseClockSource()->now() + duration));
        callback(ProducerConsumerQueueTestHelper<Milliseconds>(duration));
        callback(ProducerConsumerQueueTestHelper<Date_t>(
            _serviceCtx->getPreciseClockSource()->now() + duration));
    }

private:
    std::unique_ptr<ServiceContext> _serviceCtx;
};

class MoveOnly {
public:
    struct CostFunc {
        CostFunc() = default;
        explicit CostFunc(size_t val) : val(val) {}

        size_t operator()(const MoveOnly& mo) const {
            return val + *mo._val;
        }

        const size_t val = 0;
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

TEST_F(ProducerConsumerQueueTest, basicPushPop) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{};

        helper
            .runThread(
                "Producer",
                [&](auto... interruptionArgs) { pcq.push(MoveOnly(1), interruptionArgs...); })
            .join();

        ASSERT_EQUALS(pcq.sizeForTest(), 1ul);

        helper
            .runThread("Consumer",
                       [&](auto... interruptionArgs) {
                           ASSERT_EQUALS(pcq.pop(interruptionArgs...), MoveOnly(1));
                       })
            .join();

        ASSERT_TRUE(pcq.emptyForTest());
    });
}

TEST_F(ProducerConsumerQueueTest, closeConsumerEnd) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{1};

        pcq.push(MoveOnly(1));

        auto producer = helper.runThread("Producer", [&](auto... interruptionArgs) {
            ASSERT_THROWS_CODE(pcq.push(MoveOnly(2), interruptionArgs...),
                               DBException,
                               ErrorCodes::ProducerConsumerQueueEndClosed);
        });

        ASSERT_EQUALS(pcq.sizeForTest(), 1ul);

        pcq.closeConsumerEnd();

        ASSERT_THROWS_CODE(pcq.pop(), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);

        producer.join();
    });
}

TEST_F(ProducerConsumerQueueTest, closeProducerEndImmediate) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{};

        pcq.push(MoveOnly(1));
        pcq.closeProducerEnd();

        helper
            .runThread("Consumer",
                       [&](auto... interruptionArgs) {
                           ASSERT_EQUALS(pcq.pop(interruptionArgs...), MoveOnly(1));

                           ASSERT_THROWS_CODE(pcq.pop(interruptionArgs...),
                                              DBException,
                                              ErrorCodes::ProducerConsumerQueueEndClosed);
                       })
            .join();

    });
}

TEST_F(ProducerConsumerQueueTest, closeProducerEndBlocking) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{};

        auto consumer = helper.runThread("Consumer", [&](auto... interruptionArgs) {
            ASSERT_THROWS_CODE(pcq.pop(interruptionArgs...),
                               DBException,
                               ErrorCodes::ProducerConsumerQueueEndClosed);
        });

        pcq.closeProducerEnd();

        consumer.join();
    });
}

TEST_F(ProducerConsumerQueueTest, popsWithTimeout) {
    runTimeoutPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{};

        helper
            .runThread(
                "Consumer",
                [&](auto... interruptionArgs) {
                    ASSERT_THROWS_CODE(
                        pcq.pop(interruptionArgs...), DBException, ErrorCodes::ExceededTimeLimit);

                    std::vector<MoveOnly> vec;
                    ASSERT_THROWS_CODE(pcq.popMany(std::back_inserter(vec), interruptionArgs...),
                                       DBException,
                                       ErrorCodes::ExceededTimeLimit);

                    ASSERT_THROWS_CODE(
                        pcq.popManyUpTo(1000, std::back_inserter(vec), interruptionArgs...),
                        DBException,
                        ErrorCodes::ExceededTimeLimit);
                })
            .join();

        ASSERT_EQUALS(pcq.sizeForTest(), 0ul);
    });
}

TEST_F(ProducerConsumerQueueTest, pushesWithTimeout) {
    runTimeoutPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{1};

        {
            MoveOnly mo(1);
            pcq.push(std::move(mo));
            ASSERT(mo.movedFrom());
        }

        helper
            .runThread("Consumer",
                       [&](auto... interruptionArgs) {
                           {
                               MoveOnly mo(2);
                               ASSERT_THROWS_CODE(pcq.push(std::move(mo), interruptionArgs...),
                                                  DBException,
                                                  ErrorCodes::ExceededTimeLimit);
                               ASSERT_EQUALS(pcq.sizeForTest(), 1ul);
                               ASSERT(!mo.movedFrom());
                               ASSERT_EQUALS(mo, MoveOnly(2));
                           }

                           {
                               std::vector<MoveOnly> vec;
                               vec.emplace_back(MoveOnly(2));

                               auto iter = begin(vec);
                               ASSERT_THROWS_CODE(pcq.pushMany(iter, end(vec), interruptionArgs...),
                                                  DBException,
                                                  ErrorCodes::ExceededTimeLimit);
                               ASSERT_EQUALS(pcq.sizeForTest(), 1ul);
                               ASSERT(!vec[0].movedFrom());
                               ASSERT_EQUALS(vec[0], MoveOnly(2));
                           }
                       })
            .join();

        ASSERT_EQUALS(pcq.sizeForTest(), 1ul);
    });
}

TEST_F(ProducerConsumerQueueTest, basicPushPopWithBlocking) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{};

        auto consumer = helper.runThread("Consumer", [&](auto... interruptionArgs) {
            ASSERT_EQUALS(pcq.pop(interruptionArgs...), MoveOnly(1));
        });

        auto producer = helper.runThread("Producer", [&](auto... interruptionArgs) {
            pcq.push(MoveOnly(1), interruptionArgs...);
        });

        consumer.join();
        producer.join();

        ASSERT_TRUE(pcq.emptyForTest());
    });
}

TEST_F(ProducerConsumerQueueTest, multipleStepPushPopWithBlocking) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{1};

        auto consumer = helper.runThread("Consumer", [&](auto... interruptionArgs) {
            for (int i = 0; i < 10; ++i) {
                ASSERT_EQUALS(pcq.pop(interruptionArgs...), MoveOnly(i));
            }
        });

        auto producer = helper.runThread("Producer", [&](auto... interruptionArgs) {
            for (int i = 0; i < 10; ++i) {
                pcq.push(MoveOnly(i), interruptionArgs...);
            }
        });

        consumer.join();
        producer.join();

        ASSERT_TRUE(pcq.emptyForTest());
    });
}


TEST_F(ProducerConsumerQueueTest, pushTooLarge) {
    runPermutations([](auto helper) {
        {
            ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{1};

            helper
                .runThread("Producer",
                           [&](auto... interruptionArgs) {
                               ASSERT_THROWS_CODE(pcq.push(MoveOnly(2), interruptionArgs...),
                                                  DBException,
                                                  ErrorCodes::ProducerConsumerQueueBatchTooLarge);
                           })
                .join();
        }

        {
            ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{4};

            std::vector<MoveOnly> vec;
            vec.push_back(MoveOnly(3));
            vec.push_back(MoveOnly(3));

            helper
                .runThread("Producer",
                           [&](auto... interruptionArgs) {
                               ASSERT_THROWS_CODE(
                                   pcq.pushMany(begin(vec), end(vec), interruptionArgs...),
                                   DBException,
                                   ErrorCodes::ProducerConsumerQueueBatchTooLarge);
                           })
                .join();
        }
    });
}

TEST_F(ProducerConsumerQueueTest, pushManyPopWithoutBlocking) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{};

        helper
            .runThread("Producer",
                       [&](auto... interruptionArgs) {
                           std::vector<MoveOnly> vec;
                           for (int i = 0; i < 10; ++i) {
                               vec.emplace_back(MoveOnly(i));
                           }

                           pcq.pushMany(begin(vec), end(vec), interruptionArgs...);
                       })
            .join();

        helper
            .runThread("Consumer",
                       [&](auto... interruptionArgs) {
                           for (int i = 0; i < 10; ++i) {
                               ASSERT_EQUALS(pcq.pop(interruptionArgs...), MoveOnly(i));
                           }
                       })
            .join();

        ASSERT_TRUE(pcq.emptyForTest());
    });
}

TEST_F(ProducerConsumerQueueTest, popManyPopWithBlocking) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{2};

        auto consumer = helper.runThread("Consumer", [&](auto... interruptionArgs) {
            for (int i = 0; i < 10; i = i + 2) {
                std::vector<MoveOnly> out;

                pcq.popMany(std::back_inserter(out), interruptionArgs...);

                ASSERT_EQUALS(out.size(), 2ul);
                ASSERT_EQUALS(out[0], MoveOnly(i));
                ASSERT_EQUALS(out[1], MoveOnly(i + 1));
            }
        });

        auto producer = helper.runThread("Producer", [&](auto... interruptionArgs) {
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

        ASSERT_TRUE(pcq.emptyForTest());
    });
}

TEST_F(ProducerConsumerQueueTest, popManyUpToPopWithBlocking) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{4};

        auto consumer = helper.runThread("Consumer", [&](auto... interruptionArgs) {
            for (int i = 0; i < 10; i = i + 2) {
                std::vector<MoveOnly> out;

                size_t spent;
                std::tie(spent, std::ignore) =
                    pcq.popManyUpTo(2, std::back_inserter(out), interruptionArgs...);

                ASSERT_EQUALS(spent, 2ul);
                ASSERT_EQUALS(out.size(), 2ul);
                ASSERT_EQUALS(out[0], MoveOnly(i));
                ASSERT_EQUALS(out[1], MoveOnly(i + 1));
            }
        });

        auto producer = helper.runThread("Producer", [&](auto... interruptionArgs) {
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

        ASSERT_TRUE(pcq.emptyForTest());
    });
}

TEST_F(ProducerConsumerQueueTest, popManyUpToPopWithBlockingWithSpecialCost) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{};

        auto consumer = helper.runThread("Consumer", [&](auto... interruptionArgs) {
            {
                std::vector<MoveOnly> out;
                size_t spent;
                std::tie(spent, std::ignore) =
                    pcq.popManyUpTo(5, std::back_inserter(out), interruptionArgs...);

                ASSERT_EQUALS(spent, 6ul);
                ASSERT_EQUALS(out.size(), 3ul);
                ASSERT_EQUALS(out[0], MoveOnly(1));
                ASSERT_EQUALS(out[1], MoveOnly(2));
                ASSERT_EQUALS(out[2], MoveOnly(3));
            }

            {
                std::vector<MoveOnly> out;
                size_t spent;
                std::tie(spent, std::ignore) =
                    pcq.popManyUpTo(15, std::back_inserter(out), interruptionArgs...);

                ASSERT_EQUALS(spent, 9ul);
                ASSERT_EQUALS(out.size(), 2ul);
                ASSERT_EQUALS(out[0], MoveOnly(4));
                ASSERT_EQUALS(out[1], MoveOnly(5));
            }
        });

        auto producer = helper.runThread("Producer", [&](auto... interruptionArgs) {
            std::vector<MoveOnly> vec;
            for (int i = 1; i < 6; ++i) {
                vec.emplace_back(MoveOnly(i));
            }

            pcq.pushMany(begin(vec), end(vec), interruptionArgs...);
        });

        consumer.join();
        producer.join();

        ASSERT_TRUE(pcq.emptyForTest());
    });
}

TEST_F(ProducerConsumerQueueTest, singleProducerMultiConsumer) {
    runPermutations([](auto helper) {
        ProducerConsumerQueue<MoveOnly> pcq{};

        stdx::mutex mutex;
        size_t success = 0;
        size_t failure = 0;

        std::array<stdx::thread, 3> threads;
        for (auto& thread : threads) {
            thread = helper.runThread("Consumer", [&](auto... interruptionArgs) {
                {
                    try {
                        pcq.pop(interruptionArgs...);
                        stdx::lock_guard<stdx::mutex> lk(mutex);
                        success++;
                    } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>&) {
                        stdx::lock_guard<stdx::mutex> lk(mutex);
                        failure++;
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

        ASSERT_EQUALS(success, 2ul);
        ASSERT_EQUALS(failure, 1ul);

        ASSERT_TRUE(pcq.emptyForTest());
    });
}

TEST_F(ProducerConsumerQueueTest, basicTryPop) {
    ProducerConsumerQueue<MoveOnly> pcq{};

    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_EQUALS(pcq.sizeForTest(), 1ul);

    auto val = pcq.tryPop();

    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val);
    ASSERT_EQUALS(*val, MoveOnly(1));

    ASSERT_TRUE(pcq.emptyForTest());
}

TEST_F(ProducerConsumerQueueTest, basicTryPush) {
    ProducerConsumerQueue<MoveOnly> pcq{1};

    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_FALSE(pcq.tryPush(MoveOnly(2)));

    ASSERT_EQUALS(pcq.sizeForTest(), 1ul);

    auto val = pcq.tryPop();
    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val);
    ASSERT_EQUALS(*val, MoveOnly(1));

    ASSERT_TRUE(pcq.emptyForTest());
}

TEST_F(ProducerConsumerQueueTest, tryPushWithSpecialCost) {
    ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{5};

    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_TRUE(pcq.tryPush(MoveOnly(2)));
    ASSERT_FALSE(pcq.tryPush(MoveOnly(3)));

    ASSERT_EQUALS(pcq.sizeForTest(), 3ul);

    auto val1 = pcq.tryPop();
    ASSERT_EQUALS(pcq.sizeForTest(), 2ul);
    auto val2 = pcq.tryPop();
    ASSERT_EQUALS(pcq.sizeForTest(), 0ul);
    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val1);
    ASSERT_TRUE(val2);
    ASSERT_EQUALS(*val1, MoveOnly(1));
    ASSERT_EQUALS(*val2, MoveOnly(2));

    ASSERT_TRUE(pcq.emptyForTest());
}

TEST_F(ProducerConsumerQueueTest, tryPushWithSpecialStatefulCost) {
    ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{5, MoveOnly::CostFunc(1)};

    ASSERT_TRUE(pcq.tryPush(MoveOnly(1)));
    ASSERT_TRUE(pcq.tryPush(MoveOnly(2)));
    ASSERT_FALSE(pcq.tryPush(MoveOnly(3)));

    ASSERT_EQUALS(pcq.sizeForTest(), 5ul);

    auto val1 = pcq.tryPop();
    ASSERT_EQUALS(pcq.sizeForTest(), 3ul);
    auto val2 = pcq.tryPop();
    ASSERT_EQUALS(pcq.sizeForTest(), 0ul);
    ASSERT_FALSE(pcq.tryPop());
    ASSERT_TRUE(val1);
    ASSERT_TRUE(val2);
    ASSERT_EQUALS(*val1, MoveOnly(1));
    ASSERT_EQUALS(*val2, MoveOnly(2));

    ASSERT_TRUE(pcq.emptyForTest());
}

}  // namespace

}  // namespace mongo
