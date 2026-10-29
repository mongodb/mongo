// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_conflict_backoff.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <limits>

namespace mongo {
namespace {

using write_conflict_backoff::backoffBaseMillis;

// Default knob values: rampStart 8ms, cap 256ms; growth is fixed at x4.
int64_t defaultSchedule(size_t n) {
    return backoffBaseMillis(n, 8, 256);
}

TEST(WriteConflictBackoff, FirstAttemptHasNoDelay) {
    ASSERT_EQ(defaultSchedule(0), 0);
    ASSERT_EQ(defaultSchedule(1), 0);
}

TEST(WriteConflictBackoff, FastAttemptsTwoAndThree) {
    ASSERT_EQ(defaultSchedule(2), 1);
    ASSERT_EQ(defaultSchedule(3), 2);
}

TEST(WriteConflictBackoff, GeometricRampFromAttemptFour) {
    ASSERT_EQ(defaultSchedule(4), 8);
    ASSERT_EQ(defaultSchedule(5), 32);
    ASSERT_EQ(defaultSchedule(6), 128);
}

TEST(WriteConflictBackoff, CapsAtCapMs) {
    ASSERT_EQ(defaultSchedule(7), 256);
    ASSERT_EQ(defaultSchedule(8), 256);
    ASSERT_EQ(defaultSchedule(1000), 256);
    ASSERT_EQ(defaultSchedule(std::numeric_limits<size_t>::max()), 256);
}

TEST(WriteConflictBackoff, HonorsCustomRampParameters) {
    // Slow ramp: 1ms start, 1s cap (growth fixed at x4).
    ASSERT_EQ(backoffBaseMillis(4, 1, 1000), 1);
    ASSERT_EQ(backoffBaseMillis(7, 1, 1000), 64);
    ASSERT_EQ(backoffBaseMillis(9, 1, 1000), 1000);
    // Cap below the fast attempts clamps them too.
    ASSERT_EQ(backoffBaseMillis(2, 8, 1), 1);
    ASSERT_EQ(backoffBaseMillis(3, 8, 1), 1);
    ASSERT_EQ(backoffBaseMillis(4, 8, 1), 1);
}

// The new path sleeps via opCtx->sleepFor (interruptible); the RampStartMs=0 fallback routes to
// the legacy logWriteConflictAndBackoff whose sleep never checks interruption. A killed opCtx
// therefore distinguishes the two paths without timing assertions.
class WriteConflictBackoffKillSwitchTest : public ServiceContextTest {
protected:
    struct KnobGuard {
        long long saved = internalQueryWriteConflictBackoffRampStartMs.load();
        ~KnobGuard() {
            internalQueryWriteConflictBackoffRampStartMs.store(saved);
        }
    };
};

TEST_F(WriteConflictBackoffKillSwitchTest, NewPathSleepIsInterruptible) {
    KnobGuard guard;
    internalQueryWriteConflictBackoffRampStartMs.store(8);
    auto opCtx = makeOperationContext();
    opCtx->markKilled(ErrorCodes::Interrupted);
    const auto nss = NamespaceString::createNamespaceString_forTest("test.backoff");
    // Attempt 2 sleeps (1ms base), so the killed opCtx interrupts the sleep.
    ASSERT_THROWS_CODE(write_conflict_backoff::logAndBackoff(
                           opCtx.get(), 2, "test op", "", NamespaceStringOrUUID(nss)),
                       DBException,
                       ErrorCodes::Interrupted);
}

TEST_F(WriteConflictBackoffKillSwitchTest, RampStartZeroFallsBackToLegacyUninterruptibleBackoff) {
    KnobGuard guard;
    internalQueryWriteConflictBackoffRampStartMs.store(0);
    auto opCtx = makeOperationContext();
    opCtx->markKilled(ErrorCodes::Interrupted);
    const auto nss = NamespaceString::createNamespaceString_forTest("test.backoff");
    // The legacy stepped backoff (0ms below 4 attempts) ignores the killed opCtx and returns.
    write_conflict_backoff::logAndBackoff(
        opCtx.get(), 2, "test op", "", NamespaceStringOrUUID(nss));
}

}  // namespace
}  // namespace mongo
