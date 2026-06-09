/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/plan_executor_impl.h"

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
// WriteConflictStreakGuard RAII contract tests.
//

TEST(WriteConflictStreakGuard, StaticInvariants) {
    // Defends the RAII contract against future refactors that would otherwise silently
    // re-enable copy / move and break double-decrement-on-destruction.
    static_assert(!std::is_copy_constructible_v<PlanExecutorImpl::WriteConflictStreakGuard>);
    static_assert(!std::is_copy_assignable_v<PlanExecutorImpl::WriteConflictStreakGuard>);
    static_assert(!std::is_move_constructible_v<PlanExecutorImpl::WriteConflictStreakGuard>);
    static_assert(!std::is_move_assignable_v<PlanExecutorImpl::WriteConflictStreakGuard>);
}

TEST(WriteConflictStreakGuard, ConstructDoesNotTouchGauge) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    {
        PlanExecutorImpl::WriteConflictStreakGuard guard;
        ASSERT_FALSE(guard.held());
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
    }
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WriteConflictStreakGuard, AcquireIncrementsAndDestructorReleases) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    {
        PlanExecutorImpl::WriteConflictStreakGuard guard;
        guard.acquire();
        ASSERT_TRUE(guard.held());
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
    }
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WriteConflictStreakGuard, ExplicitReleaseDecrements) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    PlanExecutorImpl::WriteConflictStreakGuard guard;
    guard.acquire();
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
    guard.release();
    ASSERT_FALSE(guard.held());
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
    // Destructor must be a no-op after explicit release (no double-decrement).
}

TEST(WriteConflictStreakGuard, AcquireIsIdempotent) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    PlanExecutorImpl::WriteConflictStreakGuard guard;
    guard.acquire();
    guard.acquire();
    guard.acquire();
    ASSERT_TRUE(guard.held());
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
    guard.release();
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WriteConflictStreakGuard, ReleaseWithoutAcquireIsNoOp) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    PlanExecutorImpl::WriteConflictStreakGuard guard;
    guard.release();
    guard.release();
    ASSERT_FALSE(guard.held());
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WriteConflictStreakGuard, ExceptionUnwindReleasesGauge) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    try {
        PlanExecutorImpl::WriteConflictStreakGuard guard;
        guard.acquire();
        ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before + 1);
        throw std::runtime_error("simulated WCE-limit throw");
    } catch (const std::runtime_error&) {
        // Guard destructor must have decremented during stack unwind.
    }
    ASSERT_EQ(PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest(), before);
}

TEST(WriteConflictStreakGuard, MultipleConcurrentInstancesEachOwnSlot) {
    const int32_t before = PlanExecutorImpl::getWriteConflictRetryWaiterCount_forTest();
    {
        PlanExecutorImpl::WriteConflictStreakGuard a;
        PlanExecutorImpl::WriteConflictStreakGuard b;
        PlanExecutorImpl::WriteConflictStreakGuard c;
        a.acquire();
        b.acquire();
        c.acquire();
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
