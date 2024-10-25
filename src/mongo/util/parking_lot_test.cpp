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

#include <cstddef>
#include <memory>

#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/parking_lot.h"

namespace mongo {
namespace {
using unittest::JoinThread;

class MockNotifiable final : public Notifiable {
public:
    void notify() noexcept override {
        notified.store(true);
    }

    bool isNotified() const {
        return notified.load();
    }

    // Simulate work until Notifiable has been notified.
    void run() {
        while (!notified.load()) {
            stdx::this_thread::yield();
        }
    }

private:
    Atomic<bool> notified{false};
};

class ParkingLotTest : public unittest::Test {
protected:
    ParkingLot parkingLot;
};

TEST_F(ParkingLotTest, UnparkOneEmpty) {
    ASSERT_FALSE(parkingLot.unparkOne());
}

TEST_F(ParkingLotTest, UnparkOneNotifiableExecutesCallback) {
    // Case 1: Notifiable is unblocked via ParkingLot::unparkOne
    MockNotifiable notifiable;
    bool executedCallback = false;
    auto barrier = std::make_shared<unittest::Barrier>(2);
    JoinThread parkingThread([&]() {
        parkingLot.parkOne(notifiable, [&]() noexcept {
            executedCallback = true;
            barrier->countDownAndWait();
            notifiable.run();
        });
    });

    // Wait for parkingThread to park its notifiable and begin executing callback
    barrier->countDownAndWait();
    ASSERT_TRUE(notifiable.getHandleContainer().empty());

    parkingLot.unparkOne();
    parkingThread.join();

    ASSERT_TRUE(notifiable.isNotified());
    ASSERT_TRUE(executedCallback);
    ASSERT_FALSE(notifiable.getHandleContainer().empty());
}

TEST_F(ParkingLotTest, UnblockOneNotifiableViaNotify) {
    // Case 2: Notifiable is unblocked via Notifable::notify
    MockNotifiable notifiable;
    auto barrier = std::make_shared<unittest::Barrier>(2);
    JoinThread parkingThread([&]() {
        parkingLot.parkOne(notifiable, [&]() noexcept {
            barrier->countDownAndWait();
            notifiable.run();
        });
    });
    barrier->countDownAndWait();
    notifiable.notify();
    parkingThread.join();

    ASSERT_TRUE(notifiable.isNotified());
    ASSERT_FALSE(notifiable.getHandleContainer().empty());
}

TEST_F(ParkingLotTest, UnparkAllNotifiables) {
    const size_t numNotifiables = 5;
    std::vector<MockNotifiable> notifiables(numNotifiables);
    std::vector<JoinThread> threads;
    auto barrier = std::make_shared<unittest::Barrier>(numNotifiables + 1);

    for (size_t i = 0; i < numNotifiables; ++i) {
        threads.emplace_back([&, i]() {
            parkingLot.parkOne(notifiables[i], [&, i]() noexcept {
                barrier->countDownAndWait();
                notifiables[i].run();
            });
        });
    }

    barrier->countDownAndWait();
    parkingLot.unparkAll();

    for (auto& thread : threads) {
        thread.join();
    }

    for (const auto& notifiable : notifiables) {
        ASSERT_TRUE(notifiable.isNotified());
    }
}

TEST_F(ParkingLotTest, ConcurrentParkNotifiablesWithSequentialUnpark) {
    // Test ParkingLot thread safety by parking Notifiables concurrently and unparking sequentially
    const size_t numNotifiables = 100;
    std::vector<MockNotifiable> notifiables(numNotifiables);
    std::vector<JoinThread> threads;
    auto barrier = std::make_shared<unittest::Barrier>(numNotifiables + 1);

    for (size_t i = 0; i < numNotifiables; ++i) {
        threads.emplace_back([&, i]() {
            parkingLot.parkOne(notifiables[i], [&, i]() noexcept {
                barrier->countDownAndWait();
                notifiables[i].run();
            });
        });
    }

    barrier->countDownAndWait();
    for (size_t i = 0; i < numNotifiables; ++i) {
        ASSERT_TRUE(parkingLot.unparkOne());
    }

    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_FALSE(parkingLot.unparkOne());
}

TEST_F(ParkingLotTest, UnparkOneOfManyNotifiablesIsFIFO) {
    MockNotifiable notifiable1, notifiable2;

    auto firstParkedBarrier = std::make_shared<unittest::Barrier>(2);
    auto bothParkedBarrier = std::make_shared<unittest::Barrier>(3);

    JoinThread parkingThread1([&]() {
        parkingLot.parkOne(notifiable1, [&]() noexcept {
            firstParkedBarrier->countDownAndWait();
            bothParkedBarrier->countDownAndWait();
            notifiable1.run();
        });
    });
    JoinThread parkingThread2([&]() {
        firstParkedBarrier->countDownAndWait();
        parkingLot.parkOne(notifiable2, [&]() noexcept {
            bothParkedBarrier->countDownAndWait();
            notifiable2.run();
        });
    });

    bothParkedBarrier->countDownAndWait();
    parkingLot.unparkOne();
    parkingThread1.join();

    ASSERT_FALSE(notifiable1.getHandleContainer().empty());
    ASSERT_TRUE(notifiable2.getHandleContainer().empty());

    // Clean up to unblock parkingThread2, which is still waiting for notification
    parkingLot.unparkOne();
    parkingThread2.join();
}

TEST_F(ParkingLotTest, NotifyNotifiableBeforeParking) {
    MockNotifiable notifiable;
    auto barrier = std::make_shared<unittest::Barrier>(2);
    bool executedCallback = false;

    // An already-notified Notifiable must unblock without being unparked or re-notified
    notifiable.notify();
    JoinThread parkingThread([&]() {
        parkingLot.parkOne(notifiable, [&]() noexcept {
            executedCallback = true;
            barrier->countDownAndWait();
            notifiable.run();
        });
    });

    barrier->countDownAndWait();
    parkingThread.join();
    ASSERT_TRUE(executedCallback);
    ASSERT_FALSE(notifiable.getHandleContainer().empty());
}

TEST_F(ParkingLotTest, NotifyNotifiableMiddleParkingLot) {
    MockNotifiable notifiable1, notifiable2, notifiable3;

    // Ensure all notifiables are enqueued in order
    auto firstParkedBarrier = std::make_shared<unittest::Barrier>(2);
    auto secondParkedBarrier = std::make_shared<unittest::Barrier>(2);
    auto allParkedBarrier = std::make_shared<unittest::Barrier>(2);

    JoinThread parkingThread1([&]() {
        parkingLot.parkOne(notifiable1, [&]() noexcept {
            firstParkedBarrier->countDownAndWait();
            notifiable1.run();
        });
    });

    JoinThread parkingThread2([&]() {
        firstParkedBarrier->countDownAndWait();
        parkingLot.parkOne(notifiable2, [&]() noexcept {
            secondParkedBarrier->countDownAndWait();
            notifiable2.run();
        });
    });

    JoinThread parkingThread3([&]() {
        secondParkedBarrier->countDownAndWait();
        parkingLot.parkOne(notifiable3, [&]() noexcept {
            allParkedBarrier->countDownAndWait();
            notifiable3.run();
        });
    });

    allParkedBarrier->countDownAndWait();

    // Confirm notified Notifiable is spliced from middle of ParkingLot while others remain
    notifiable2.notify();
    parkingThread2.join();
    ASSERT_FALSE(notifiable2.getHandleContainer().empty());
    ASSERT_TRUE(notifiable1.getHandleContainer().empty());
    ASSERT_TRUE(notifiable3.getHandleContainer().empty());

    // Unpark dequeues in FIFO order for remaining 2 notifiables
    parkingLot.unparkOne();
    parkingThread1.join();
    ASSERT_FALSE(notifiable1.getHandleContainer().empty());

    parkingLot.unparkOne();
    parkingThread3.join();
    ASSERT_FALSE(notifiable3.getHandleContainer().empty());
    ASSERT_FALSE(parkingLot.unparkOne());
}
}  // namespace
}  // namespace mongo
