// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/parking_lot.h"

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>
#include <thread>

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
            std::this_thread::yield();
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
