/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/observable_mutex.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstdint>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

namespace {

using observable_mutex_details::AcquisitionStats;
using observable_mutex_details::ImplementsLockShared;
using observable_mutex_details::LockStats;
using observable_mutex_details::ObservationToken;

class ObservationTokenTest : public unittest::Test {
public:
    ObservationTokenTest()
        : obsToken(ObservationToken([this](LockStats& stats) {
              auto append = [](auto& agg, auto& sample) {
                  agg.total += sample.total;
                  agg.contentions += sample.contentions;
                  agg.waitCycles += sample.waitCycles;
              };
              append(stats.exclusiveAcquisitions, _tokenStats.exclusiveAcquisitions);
              append(stats.sharedAcquisitions, _tokenStats.sharedAcquisitions);
          })) {};

    void setTokenStats(LockStats& newStats) {
        _tokenStats = newStats;
    }

    const LockStats kZeroStats = {{0, 0, 0}, {0, 0, 0}};

    void verifyAcqStats(const AcquisitionStats<uint64_t>& current,
                        const AcquisitionStats<uint64_t>& expected) const {

        auto transformAcqStats = [](const AcquisitionStats<uint64_t>& stats) {
            return std::tie(stats.total, stats.contentions, stats.waitCycles);
        };

        auto actualV = transformAcqStats(current);
        auto expectedV = transformAcqStats(expected);
        ASSERT_EQ(actualV, expectedV);
    }

    void verifyLockStats(const LockStats& current, const LockStats& expected) const {
        verifyAcqStats(current.exclusiveAcquisitions, expected.exclusiveAcquisitions);
        verifyAcqStats(current.sharedAcquisitions, expected.sharedAcquisitions);
    }

    ObservationToken obsToken;

private:
    // The following mimics the metrics collected by an observable mutex.
    LockStats _tokenStats;
};

class InvalidObservationTokenTest : public ObservationTokenTest {
public:
    void setUp() override {
        ObservationTokenTest::setUp();
        obsToken.invalidate();
    }
};

TEST_F(ObservationTokenTest, Validity) {
    ASSERT(obsToken.isValid());
}

TEST_F(ObservationTokenTest, ValidInitStats) {
    verifyLockStats(*obsToken.getStats(), kZeroStats);
}

TEST_F(ObservationTokenTest, ValidGetStats) {
    // Mimic a process updating the token's stats.
    LockStats newStats = {{1, 1, 1}, {1, 1, 1}};
    setTokenStats(newStats);

    // Check that retrieving the token's stats shows updated stats.
    auto fetchedTokenStats = obsToken.getStats();
    ASSERT_TRUE(fetchedTokenStats);
    verifyLockStats(*fetchedTokenStats, newStats);
}

TEST_F(InvalidObservationTokenTest, Invalidity) {
    ASSERT_FALSE(obsToken.isValid());
}

TEST_F(InvalidObservationTokenTest, InvalidInitStats) {
    ASSERT_FALSE(obsToken.getStats());
}

TEST_F(ObservationTokenTest, ValidThenInvalid) {
    // Mimic a process updating the token's stats.
    LockStats firstUpdate = {{1, 1, 1}, {1, 1, 1}};
    setTokenStats(firstUpdate);

    // Check that retrieving the token's stats shows updated stats.
    auto fetchedTokenStats = obsToken.getStats();
    ASSERT_TRUE(fetchedTokenStats);
    verifyLockStats(*fetchedTokenStats, firstUpdate);

    // Invalidate the token.
    obsToken.invalidate();

    // Check that retrieving the token's stats returns an optional.
    ASSERT_FALSE(obsToken.getStats());
}

class ObservableMutexTest : public ObservationTokenTest {
public:
    class ExclusiveLocker {
    public:
        template <typename MutexType>
        void lock(MutexType& m) {
            m.lock();
        }

        template <typename MutexType>
        void unlock(MutexType& m) {
            m.unlock();
        }
    };

    class SharedLocker {
    public:
        template <typename MutexType>
        void lock(MutexType& m) {
            m.lock_shared();
        }

        template <typename MutexType>
        void unlock(MutexType& m) {
            m.unlock_shared();
        }
    };

    template <typename MutexType>
    void waitForChange(MutexType& m, const LockStats& oldStats) {
        size_t kNumChecks = 50;
        while (kNumChecks--) {
            auto currentValue = *m.token()->getStats();
            if (!(currentValue.exclusiveAcquisitions.contentions ==
                  oldStats.exclusiveAcquisitions.contentions) ||
                !(currentValue.sharedAcquisitions.contentions ==
                  oldStats.sharedAcquisitions.contentions)) {
                return;
            }
            sleepFor(Milliseconds(1));
        }
        ASSERT(false) << "Timed out waiting for lock stats to change.";
    }

    template <typename MutexType, typename Locker>
    void statsTest(MutexType& m, Locker locker) {
        boost::optional<unittest::JoinThread> waiter;
        stdx::lock_guard lk(m);
        LockStats expected = LockStats{{1, 0, 0}, {0, 0, 0}};
        auto fetchedTokenStats = m.token()->getStats();
        ASSERT_TRUE(fetchedTokenStats);
        verifyLockStats(*fetchedTokenStats, expected);
        waiter.emplace([&] {
            locker.lock(m);
            locker.unlock(m);
        });
        waitForChange(m, expected);
    }

    template <typename MutexType, typename Locker>
    void throwTest(MutexType& m, Locker locker) {
        boost::optional<unittest::JoinThread> waiter;
        Notification<void> waiterThrew;
        stdx::lock_guard lk(m);
        waiter.emplace([&] {
            try {
                locker.lock(m);  // Expected to throw.
            } catch (const DBException&) {
                waiterThrew.set();
            }
        });
        waiter->join();
        ASSERT_TRUE(waiterThrew);
    }
};

// Mocks the lock functions to simulate a throw for testing.
template <typename MutexType>
class ThrowingMutex {
public:
    void lock() {
        if (!_m.try_lock()) {
            auto reason = "Simulated lock failure for test";
            uasserted(ErrorCodes::LockFailed, reason);
        }
    }

    bool try_lock() {
        return _m.try_lock();
    }

    void unlock() {
        _m.unlock();
    }

    void lock_shared()
    requires ImplementsLockShared<MutexType>
    {
        if (!_m.try_lock_shared()) {
            auto reason = "Simulated lock failure for test";
            uasserted(ErrorCodes::LockFailed, reason);
        }
    }

    bool try_lock_shared()
    requires ImplementsLockShared<MutexType>
    {
        return _m.try_lock_shared();
    }

    void unlock_shared()
    requires ImplementsLockShared<MutexType>
    {
        _m.unlock_shared();
    }

private:
    MutexType _m;
};

using ThrowingObservableMutex = ObservableMutex<ThrowingMutex<std::mutex>>;
using ThrowingObservableSharedMutex = ObservableMutex<ThrowingMutex<std::shared_mutex>>;

TEST_F(ObservableMutexTest, MetricsAreInitializedToZero) {
    ObservableSharedMutex m;
    auto maybeStats = m.token()->getStats();
    ASSERT_TRUE(maybeStats);
    verifyLockStats(*maybeStats, kZeroStats);
}

TEST_F(ObservableMutexTest, AcquisitionMetricsNotUpdatedWhenAcquisitionThrows) {
    ThrowingObservableMutex m;
    throwTest(m, ExclusiveLocker{});
    LockStats expected = LockStats{{1, 0, 0}, {0, 0, 0}};
    auto fetchedTokenStats = m.token()->getStats();
    ASSERT_TRUE(fetchedTokenStats);
    verifyLockStats(*fetchedTokenStats, expected);
}

TEST_F(ObservableMutexTest, AcquisitionMetricsNotUpdatedWhenSharedAcquisitionThrows) {
    ThrowingObservableSharedMutex m;
    throwTest(m, SharedLocker{});
    LockStats expected = LockStats{{1, 0, 0}, {0, 0, 0}};
    auto fetchedTokenStats = m.token()->getStats();
    ASSERT_TRUE(fetchedTokenStats);
    verifyLockStats(*fetchedTokenStats, expected);
}

#ifndef _WIN32
TEST_F(ObservableMutexTest, AcquisitionMetricsCorrectlyUpdated) {
    ObservableExclusiveMutex m;
    statsTest(m, ExclusiveLocker{});
    AcquisitionStats expected = AcquisitionStats<uint64_t>{0, 0, 0};
    auto fetchedTokenStats = m.token()->getStats();
    ASSERT_TRUE(fetchedTokenStats);
    ASSERT_EQ((*fetchedTokenStats).exclusiveAcquisitions.total, 2);
    ASSERT_EQ((*fetchedTokenStats).exclusiveAcquisitions.contentions, 1);
    ASSERT_GREATER_THAN((*fetchedTokenStats).exclusiveAcquisitions.waitCycles, 0);
    verifyAcqStats((*fetchedTokenStats).sharedAcquisitions, expected);
}

TEST_F(ObservableMutexTest, SharedAcquisitionMetricsCorrectlyUpdated) {
    ObservableSharedMutex m;
    statsTest(m, SharedLocker{});
    AcquisitionStats expected = AcquisitionStats<uint64_t>{1, 0, 0};
    auto fetchedTokenStats = m.token()->getStats();
    ASSERT_TRUE(fetchedTokenStats);
    verifyAcqStats((*fetchedTokenStats).exclusiveAcquisitions, expected);
    ASSERT_EQ((*fetchedTokenStats).sharedAcquisitions.total, 1);
    ASSERT_EQ((*fetchedTokenStats).sharedAcquisitions.contentions, 1);
    ASSERT_GREATER_THAN((*fetchedTokenStats).sharedAcquisitions.waitCycles, 0);
}
#endif

}  // namespace
}  // namespace mongo
