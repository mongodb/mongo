// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_executor_impl.h"

#include "mongo/db/query/write_conflict_storm.h"
#include "mongo/unittest/unittest.h"

#include <stdexcept>
#include <type_traits>

namespace mongo {
namespace {

//
// adaptiveWriteConflictRetryLimit formula tests.
//
// Properties verified:
//   - Disabled when limitMax <= 0.
//   - No adaptive scaling when threshold <= 0 (returns limitMax).
//   - Linear interpolation between (0, limitMax) and (threshold, limitMin).
//   - Saturates at limitMin for waiters >= threshold.
//   - Integer-division floor at the midpoint biases slightly toward more retries.
//   - Defensive clamping of malformed limitMin (< 0 or > limitMax).
//

TEST(AdaptiveWriteConflictRetryLimit, DisabledReturnsZero) {
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(0, 3, 50, 10), 0);
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(-1, 3, 50, 10), 0);
}

TEST(AdaptiveWriteConflictRetryLimit, ThresholdZeroDisablesScaling) {
    // With threshold = 0, scaling is off: return limitMax regardless of waiters.
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 3, 0, 0), 20);
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 3, 0, 100), 20);
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 3, -1, 100), 20);
}

TEST(AdaptiveWriteConflictRetryLimit, NoWaitersReturnsMax) {
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 3, 50, 0), 20);
}

TEST(AdaptiveWriteConflictRetryLimit, SaturatedReturnsMin) {
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 3, 50, 50), 3);
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 3, 50, 1000), 3);
}

TEST(AdaptiveWriteConflictRetryLimit, MidpointIntegerFloor) {
    // At half the threshold, expected = limitMax - (limitMax - limitMin) * 25 / 50
    //                                 = 20 - 17 * 25 / 50
    //                                 = 20 - 425/50 (integer division: 8)
    //                                 = 12.
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 3, 50, 25), 12);
}

TEST(AdaptiveWriteConflictRetryLimit, LinearInterpolationMonotonic) {
    // Limit should be monotonically non-increasing as waiters grow.
    int prev = adaptiveWriteConflictRetryLimit(20, 3, 50, 0);
    for (int waiters = 1; waiters <= 60; ++waiters) {
        const int next = adaptiveWriteConflictRetryLimit(20, 3, 50, waiters);
        ASSERT_LTE(next, prev) << "Limit increased at waiters=" << waiters;
        prev = next;
    }
}

TEST(AdaptiveWriteConflictRetryLimit, NegativeMinClampedToZero) {
    // A misconfigured limitMin < 0 must be treated as 0 (not negative).
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, -5, 50, 50), 0);
}

TEST(AdaptiveWriteConflictRetryLimit, MinGreaterThanMaxClampedToMax) {
    // A misconfigured limitMin > limitMax must be clamped to limitMax (the interpolation
    // delta becomes 0 and we return limitMax for any waiters count).
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 50, 100, 0), 20);
    ASSERT_EQ(adaptiveWriteConflictRetryLimit(20, 50, 100, 100), 20);
}

//
// WCStormWaiterGuard RAII contract tests.
//

TEST(WCStormWaiterGuard, StaticInvariants) {
    // Defends the RAII contract against future refactors that would otherwise silently
    // re-enable copy / move and break double-decrement-on-destruction.
    static_assert(!std::is_copy_constructible_v<WCStormWaiterGuard>);
    static_assert(!std::is_copy_assignable_v<WCStormWaiterGuard>);
    static_assert(!std::is_move_constructible_v<WCStormWaiterGuard>);
    static_assert(!std::is_move_assignable_v<WCStormWaiterGuard>);
}

TEST(WCStormWaiterGuard, ConstructDoesNotTouchGauge) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    {
        WCStormWaiterGuard guard;
        ASSERT_FALSE(guard.active());
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
    }
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WCStormWaiterGuard, ActivateIncrementsAndDestructorReleases) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    {
        WCStormWaiterGuard guard;
        guard.activate();
        ASSERT_TRUE(guard.active());
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
    }
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WCStormWaiterGuard, ExplicitReleaseDecrements) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    WCStormWaiterGuard guard;
    guard.activate();
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
    guard.release();
    ASSERT_FALSE(guard.active());
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
    // Destructor must be a no-op after explicit release (no double-decrement).
}

TEST(WCStormWaiterGuard, ActivateIsIdempotent) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    WCStormWaiterGuard guard;
    guard.activate();
    guard.activate();
    guard.activate();
    ASSERT_TRUE(guard.active());
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
    guard.release();
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WCStormWaiterGuard, ReleaseWithoutActivateIsNoOp) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    WCStormWaiterGuard guard;
    guard.release();
    guard.release();
    ASSERT_FALSE(guard.active());
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WCStormWaiterGuard, ExceptionUnwindReleasesGauge) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    try {
        WCStormWaiterGuard guard;
        guard.activate();
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
        throw std::runtime_error("simulated WCE-limit throw");
    } catch (const std::runtime_error&) {
        // Guard destructor must have decremented during stack unwind.
    }
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WCStormWaiterGuard, MultipleConcurrentInstancesEachOwnSlot) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    {
        WCStormWaiterGuard a;
        WCStormWaiterGuard b;
        WCStormWaiterGuard c;
        a.activate();
        b.activate();
        c.activate();
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 3);
        b.release();
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 2);
        // Guards `a` and `c` fall out of scope at the closing brace below, which must drop
        // the gauge back to `before` via their destructors.
    }
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

}  // namespace
}  // namespace mongo
