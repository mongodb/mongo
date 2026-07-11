// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/observable_mutex.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

namespace {

class ObservationTokenTest : public unittest::Test {
public:
    ObservationTokenTest()
        : obsToken(ObservationToken([this](MutexStats& stats) {
              auto append = [](auto& agg, auto& sample) {
                  agg.total += sample.total;
                  agg.contentions += sample.contentions;
                  agg.waitCycles += sample.waitCycles;
              };
              append(stats.exclusiveAcquisitions, _tokenStats.exclusiveAcquisitions);
              append(stats.sharedAcquisitions, _tokenStats.sharedAcquisitions);
          })) {};

    void setTokenStats(MutexStats& newStats) {
        _tokenStats = newStats;
    }

    const MutexStats kZeroStats = {{0, 0, 0}, {0, 0, 0}};

    void verifyAcqStats(const MutexAcquisitionStats& current,
                        const MutexAcquisitionStats& expected) const {

        auto transformAcqStats = [](const MutexAcquisitionStats& stats) {
            return std::tie(stats.total, stats.contentions, stats.waitCycles);
        };

        auto actualV = transformAcqStats(current);
        auto expectedV = transformAcqStats(expected);
        ASSERT_EQ(actualV, expectedV);
    }

    void verifyMutexStats(const MutexStats& current, const MutexStats& expected) const {
        verifyAcqStats(current.exclusiveAcquisitions, expected.exclusiveAcquisitions);
        verifyAcqStats(current.sharedAcquisitions, expected.sharedAcquisitions);
    }

    ObservationToken obsToken;

private:
    // The following mimics the metrics collected by an observable mutex.
    MutexStats _tokenStats;
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
    verifyMutexStats(obsToken.getStats(), kZeroStats);
}

TEST_F(ObservationTokenTest, ValidGetStats) {
    // Mimic a process updating the token's stats.
    MutexStats newStats = {{1, 1, 1}, {1, 1, 1}};
    setTokenStats(newStats);

    // Check that retrieving the token's stats shows updated stats.
    auto fetchedTokenStats = obsToken.getStats();
    verifyMutexStats(fetchedTokenStats, newStats);
}

TEST_F(InvalidObservationTokenTest, Invalidity) {
    ASSERT_FALSE(obsToken.isValid());
}

TEST_F(InvalidObservationTokenTest, InvalidInitStats) {
    verifyMutexStats(obsToken.getStats(), {{0, 0, 0}, {0, 0, 0}});
}

TEST_F(ObservationTokenTest, ValidThenInvalid) {
    // Mimic a process updating the token's stats.
    MutexStats firstUpdate = {{1, 1, 1}, {1, 1, 1}};
    setTokenStats(firstUpdate);

    // Check that retrieving the token's stats shows updated stats.
    auto fetchedTokenStats = obsToken.getStats();
    verifyMutexStats(fetchedTokenStats, firstUpdate);

    // Invalidate the token.
    obsToken.invalidate();

    // Check that stats are still accessible after invalidation.
    verifyMutexStats(fetchedTokenStats, firstUpdate);
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

    /**
     * Waits for `token.getStats()` to have contention counts different from `old`.
     * Retries at geometrically increasing intervals until a hard-coded timeout.
     */
    void waitForChangeInContentions(ObservationToken& token, const MutexStats& old) {
        auto now = Date_t::now();
        const auto timeout = Seconds(10);
        const auto deadline = now + timeout;
        auto delay = Milliseconds(1);
        for (; now < deadline; now = Date_t::now(), delay *= 2) {
            const MutexStats current = token.getStats();
            if (current.exclusiveAcquisitions.contentions !=
                    old.exclusiveAcquisitions.contentions ||
                current.sharedAcquisitions.contentions != old.sharedAcquisitions.contentions) {
                return;
            }
            sleepFor(std::min(delay, deadline - now));
        }
        FAIL("contention stats did not change")
            << " (timeout=" << timeout
            << " .exclusiveAcquisitions.contentions=" << old.exclusiveAcquisitions.contentions
            << " .sharedAcquisitions.contentions=" << old.sharedAcquisitions.contentions << ')';
    }

    template <typename MutexType, typename Locker>
    void statsTest(MutexType& m, Locker locker) {
        ObservationToken& token = *m.token();
        boost::optional<unittest::JoinThread> waiter;
        std::lock_guard lk(m);
        const MutexStats expected = MutexStats{{1, 0, 0}, {0, 0, 0}};
        verifyMutexStats(token.getStats(), expected);
        waiter.emplace([&] {
            locker.lock(m);
            locker.unlock(m);
        });
        waitForChangeInContentions(token, expected);
    }

    template <typename MutexType, typename Locker>
    void throwTest(MutexType& m, Locker locker) {
        boost::optional<unittest::JoinThread> waiter;
        Notification<void> waiterThrew;
        std::lock_guard lk(m);
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

    void lock_shared() {
        if (!_m.try_lock_shared()) {
            auto reason = "Simulated lock failure for test";
            uasserted(ErrorCodes::LockFailed, reason);
        }
    }

    bool try_lock_shared() {
        return _m.try_lock_shared();
    }

    void unlock_shared() {
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
    verifyMutexStats(maybeStats, kZeroStats);
}

TEST_F(ObservableMutexTest, AcquisitionMetricsNotUpdatedWhenAcquisitionThrows) {
    ThrowingObservableMutex m;
    throwTest(m, ExclusiveLocker{});
    MutexStats expected = MutexStats{{1, 0, 0}, {0, 0, 0}};
    auto fetchedTokenStats = m.token()->getStats();
    verifyMutexStats(fetchedTokenStats, expected);
}

TEST_F(ObservableMutexTest, AcquisitionMetricsNotUpdatedWhenSharedAcquisitionThrows) {
    ThrowingObservableSharedMutex m;
    throwTest(m, SharedLocker{});
    MutexStats expected = MutexStats{{1, 0, 0}, {0, 0, 0}};
    auto fetchedTokenStats = m.token()->getStats();
    verifyMutexStats(fetchedTokenStats, expected);
}

#ifndef _WIN32
TEST_F(ObservableMutexTest, AcquisitionMetricsCorrectlyUpdated) {
    ObservableExclusiveMutex m;
    statsTest(m, ExclusiveLocker{});
    MutexAcquisitionStats expected = MutexAcquisitionStats{0, 0, 0};
    auto fetchedTokenStats = m.token()->getStats();
    ASSERT_EQ((fetchedTokenStats).exclusiveAcquisitions.total, 2);
    ASSERT_EQ((fetchedTokenStats).exclusiveAcquisitions.contentions, 1);
    ASSERT_GREATER_THAN((fetchedTokenStats).exclusiveAcquisitions.waitCycles, 0);
    verifyAcqStats((fetchedTokenStats).sharedAcquisitions, expected);
}

TEST_F(ObservableMutexTest, SharedAcquisitionMetricsCorrectlyUpdated) {
    ObservableSharedMutex m;
    statsTest(m, SharedLocker{});
    MutexAcquisitionStats expected = MutexAcquisitionStats{1, 0, 0};
    auto fetchedTokenStats = m.token()->getStats();
    verifyAcqStats((fetchedTokenStats).exclusiveAcquisitions, expected);
    ASSERT_EQ((fetchedTokenStats).sharedAcquisitions.total, 1);
    ASSERT_EQ((fetchedTokenStats).sharedAcquisitions.contentions, 1);
    ASSERT_GREATER_THAN((fetchedTokenStats).sharedAcquisitions.waitCycles, 0);
}
#endif

}  // namespace
}  // namespace mongo
