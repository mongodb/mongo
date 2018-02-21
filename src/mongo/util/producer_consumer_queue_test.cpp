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
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

class ProducerConsumerQueueTest : public unittest::Test {
public:
    ProducerConsumerQueueTest() : _serviceCtx(stdx::make_unique<ServiceContextNoop>()) {}

    template <typename Callback>
    stdx::thread runThread(StringData name, Callback&& cb) {
        return stdx::thread([this, name, cb] { cb(); });
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
    ProducerConsumerQueue<MoveOnly> pcq{};

    runThread("Producer", [&]() { pcq.push(MoveOnly(1)); }).join();

    ASSERT_EQUALS(pcq.sizeForTest(), 1ul);

    runThread("Consumer", [&]() { ASSERT_EQUALS(pcq.pop(), MoveOnly(1)); }).join();

    ASSERT_TRUE(pcq.emptyForTest());
}

TEST_F(ProducerConsumerQueueTest, closeConsumerEnd) {
    ProducerConsumerQueue<MoveOnly> pcq{1};

    pcq.push(MoveOnly(1));

    auto producer = runThread("Producer", [&]() {
        ASSERT_THROWS_CODE(
            pcq.push(MoveOnly(2)), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);
    });

    ASSERT_EQUALS(pcq.sizeForTest(), 1ul);

    pcq.closeConsumerEnd();

    ASSERT_THROWS_CODE(pcq.pop(), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);

    producer.join();
}

TEST_F(ProducerConsumerQueueTest, closeProducerEndImmediate) {
    ProducerConsumerQueue<MoveOnly> pcq{};

    pcq.push(MoveOnly(1));
    pcq.closeProducerEnd();

    runThread("Consumer", [&]() {
        ASSERT_EQUALS(pcq.pop(), MoveOnly(1));

        ASSERT_THROWS_CODE(pcq.pop(), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);
    }).join();
}

TEST_F(ProducerConsumerQueueTest, closeProducerEndBlocking) {
    ProducerConsumerQueue<MoveOnly> pcq{};

    auto consumer = runThread("Consumer", [&]() {
        ASSERT_THROWS_CODE(pcq.pop(), DBException, ErrorCodes::ProducerConsumerQueueEndClosed);
    });

    pcq.closeProducerEnd();

    consumer.join();
}

TEST_F(ProducerConsumerQueueTest, popsWithTimeout) {
    ProducerConsumerQueue<MoveOnly> pcq{};

    runThread("Consumer", [&]() {
        ASSERT_THROWS_CODE(pcq.pop(Milliseconds(100)), DBException, ErrorCodes::ExceededTimeLimit);

        std::vector<MoveOnly> vec;
        ASSERT_THROWS_CODE(pcq.popMany(std::back_inserter(vec), Milliseconds(100)),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        ASSERT_THROWS_CODE(pcq.popManyUpTo(1000, std::back_inserter(vec), Milliseconds(100)),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);
    }).join();

    ASSERT_EQUALS(pcq.sizeForTest(), 0ul);
}

TEST_F(ProducerConsumerQueueTest, pushesWithTimeout) {
    ProducerConsumerQueue<MoveOnly> pcq{1};

    {
        MoveOnly mo(1);
        pcq.push(std::move(mo));
        ASSERT(mo.movedFrom());
    }

    runThread("Consumer", [&]() {
        {
            MoveOnly mo(2);
            ASSERT_THROWS_CODE(pcq.push(std::move(mo), Milliseconds(100)),
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
            ASSERT_THROWS_CODE(pcq.pushMany(iter, end(vec), Milliseconds(100)),
                               DBException,
                               ErrorCodes::ExceededTimeLimit);
            ASSERT_EQUALS(pcq.sizeForTest(), 1ul);
            ASSERT(!vec[0].movedFrom());
            ASSERT_EQUALS(vec[0], MoveOnly(2));
        }
    }).join();

    ASSERT_EQUALS(pcq.sizeForTest(), 1ul);
}

TEST_F(ProducerConsumerQueueTest, basicPushPopWithBlocking) {
    ProducerConsumerQueue<MoveOnly> pcq{};

    auto consumer = runThread("Consumer", [&]() { ASSERT_EQUALS(pcq.pop(), MoveOnly(1)); });

    auto producer = runThread("Producer", [&]() { pcq.push(MoveOnly(1)); });

    consumer.join();
    producer.join();

    ASSERT_TRUE(pcq.emptyForTest());
}

TEST_F(ProducerConsumerQueueTest, multipleStepPushPopWithBlocking) {
    ProducerConsumerQueue<MoveOnly> pcq{1};

    auto consumer = runThread("Consumer", [&]() {
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(pcq.pop(), MoveOnly(i));
        }
    });

    auto producer = runThread("Producer", [&]() {
        for (int i = 0; i < 10; ++i) {
            pcq.push(MoveOnly(i));
        }
    });

    consumer.join();
    producer.join();

    ASSERT_TRUE(pcq.emptyForTest());
}


TEST_F(ProducerConsumerQueueTest, pushTooLarge) {
    {
        ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{1};

        runThread("Producer", [&]() {
            ASSERT_THROWS_CODE(
                pcq.push(MoveOnly(2)), DBException, ErrorCodes::ProducerConsumerQueueBatchTooLarge);
        }).join();
    }

    {
        ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{4};

        std::vector<MoveOnly> vec;
        vec.push_back(MoveOnly(3));
        vec.push_back(MoveOnly(3));

        runThread("Producer", [&]() {
            ASSERT_THROWS_CODE(pcq.pushMany(begin(vec), end(vec)),
                               DBException,
                               ErrorCodes::ProducerConsumerQueueBatchTooLarge);
        }).join();
    }
}

TEST_F(ProducerConsumerQueueTest, pushManyPopWithoutBlocking) {
    ProducerConsumerQueue<MoveOnly> pcq{};

    runThread("Producer", [&]() {
        std::vector<MoveOnly> vec;
        for (int i = 0; i < 10; ++i) {
            vec.emplace_back(MoveOnly(i));
        }

        pcq.pushMany(begin(vec), end(vec));
    }).join();

    runThread("Consumer", [&]() {
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(pcq.pop(), MoveOnly(i));
        }
    }).join();

    ASSERT_TRUE(pcq.emptyForTest());
}

TEST_F(ProducerConsumerQueueTest, popManyPopWithBlocking) {
    ProducerConsumerQueue<MoveOnly> pcq{2};

    auto consumer = runThread("Consumer", [&]() {
        for (int i = 0; i < 10; i = i + 2) {
            std::vector<MoveOnly> out;

            pcq.popMany(std::back_inserter(out));

            ASSERT_EQUALS(out.size(), 2ul);
            ASSERT_EQUALS(out[0], MoveOnly(i));
            ASSERT_EQUALS(out[1], MoveOnly(i + 1));
        }
    });

    auto producer = runThread("Producer", [&]() {
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
}

TEST_F(ProducerConsumerQueueTest, popManyUpToPopWithBlocking) {
    ProducerConsumerQueue<MoveOnly> pcq{4};

    auto consumer = runThread("Consumer", [&]() {
        for (int i = 0; i < 10; i = i + 2) {
            std::vector<MoveOnly> out;

            size_t spent;
            std::tie(spent, std::ignore) = pcq.popManyUpTo(2, std::back_inserter(out));

            ASSERT_EQUALS(spent, 2ul);
            ASSERT_EQUALS(out.size(), 2ul);
            ASSERT_EQUALS(out[0], MoveOnly(i));
            ASSERT_EQUALS(out[1], MoveOnly(i + 1));
        }
    });

    auto producer = runThread("Producer", [&]() {
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
}

TEST_F(ProducerConsumerQueueTest, popManyUpToPopWithBlockingWithSpecialCost) {
    ProducerConsumerQueue<MoveOnly, MoveOnly::CostFunc> pcq{};

    auto consumer = runThread("Consumer", [&]() {
        {
            std::vector<MoveOnly> out;
            size_t spent;
            std::tie(spent, std::ignore) = pcq.popManyUpTo(5, std::back_inserter(out));

            ASSERT_EQUALS(spent, 6ul);
            ASSERT_EQUALS(out.size(), 3ul);
            ASSERT_EQUALS(out[0], MoveOnly(1));
            ASSERT_EQUALS(out[1], MoveOnly(2));
            ASSERT_EQUALS(out[2], MoveOnly(3));
        }

        {
            std::vector<MoveOnly> out;
            size_t spent;
            std::tie(spent, std::ignore) = pcq.popManyUpTo(15, std::back_inserter(out));

            ASSERT_EQUALS(spent, 9ul);
            ASSERT_EQUALS(out.size(), 2ul);
            ASSERT_EQUALS(out[0], MoveOnly(4));
            ASSERT_EQUALS(out[1], MoveOnly(5));
        }
    });

    auto producer = runThread("Producer", [&]() {
        std::vector<MoveOnly> vec;
        for (int i = 1; i < 6; ++i) {
            vec.emplace_back(MoveOnly(i));
        }

        pcq.pushMany(begin(vec), end(vec));
    });

    consumer.join();
    producer.join();

    ASSERT_TRUE(pcq.emptyForTest());
}

TEST_F(ProducerConsumerQueueTest, singleProducerMultiConsumer) {
    ProducerConsumerQueue<MoveOnly> pcq{};

    stdx::mutex mutex;
    size_t success = 0;
    size_t failure = 0;

    std::array<stdx::thread, 3> threads;
    for (auto& thread : threads) {
        thread = runThread("Consumer", [&]() {
            {
                try {
                    pcq.pop();
                    stdx::lock_guard<stdx::mutex> lk(mutex);
                    success++;
                } catch (const DBException& exception) {
                    ASSERT_EQUALS(exception.getCode(), ErrorCodes::ProducerConsumerQueueEndClosed);
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
