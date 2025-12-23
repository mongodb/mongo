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

#include "mongo/db/s/range_deletion_recovery_tracker.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

void endTerm(RangeDeletionRecoveryTracker& tracker, RangeDeletionRecoveryTracker::Term term) {
    // notifyStartOfTerm returns an RAII type that ends the term on destruction, so we immediately
    // discard it to end the term.
    auto discard = tracker.notifyStartOfTerm(term);
}

TEST(RangeDeletionRecoveryTracker, RecoveryFutureCompletesWhenAllJobsComplete) {
    const auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    tracker.registerRecoveryJob(term);
    tracker.registerRecoveryJob(term);
    auto future = tracker.getRecoveryFuture(term);
    ASSERT_FALSE(future.isReady());
    tracker.notifyRecoveryJobComplete(term);
    ASSERT_FALSE(future.isReady());
    tracker.notifyRecoveryJobComplete(term);
    ASSERT_TRUE(future.isReady());
    auto outcome = future.get();
    ASSERT_EQ(outcome, RangeDeletionRecoveryTracker::Outcome::kComplete);
}

TEST(RangeDeletionRecoveryTracker, TrackedTermsCount) {
    RangeDeletionRecoveryTracker tracker;
    ASSERT_EQ(tracker.getTrackedTermsCount(), 0);
    tracker.registerRecoveryJob(0);
    ASSERT_EQ(tracker.getTrackedTermsCount(), 1);
    tracker.registerRecoveryJob(1);
    ASSERT_EQ(tracker.getTrackedTermsCount(), 2);
    tracker.registerRecoveryJob(2);
    ASSERT_EQ(tracker.getTrackedTermsCount(), 3);
    endTerm(tracker, 1);
    ASSERT_EQ(tracker.getTrackedTermsCount(), 1);
    endTerm(tracker, 2);
    ASSERT_EQ(tracker.getTrackedTermsCount(), 0);
}

TEST(RangeDeletionRecoveryTracker, RecoveryFutureCompletesAtEndOfTerm) {
    const auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    tracker.registerRecoveryJob(term);
    auto future = tracker.getRecoveryFuture(term);
    ASSERT_FALSE(future.isReady());
    endTerm(tracker, term);
    ASSERT_TRUE(future.isReady());
    auto outcome = future.get();
    ASSERT_EQ(outcome, RangeDeletionRecoveryTracker::Outcome::kIncomplete);
}

TEST(RangeDeletionRecoveryTracker, EndingTermCompletesOlderTermsToo) {
    const auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    tracker.registerRecoveryJob(term);
    auto future = tracker.getRecoveryFuture(term);
    ASSERT_FALSE(future.isReady());
    endTerm(tracker, term);
    ASSERT_TRUE(future.isReady());
    auto outcome = future.get();
    ASSERT_EQ(outcome, RangeDeletionRecoveryTracker::Outcome::kIncomplete);
}

TEST(RangeDeletionRecoveryTracker, AddJobForCompletedTermDoesNothing) {
    auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    endTerm(tracker, term);
    tracker.registerRecoveryJob(term);
    ASSERT_EQ(tracker.getTrackedTermsCount(), 0);
}

DEATH_TEST(RangeDeletionRecoveryTracker,
           NotifyMoreJobsThanRegisteredAsserts,
           "Tripwire assertion") {
    RangeDeletionRecoveryTracker tracker;
    tracker.notifyRecoveryJobComplete(0);
}

TEST(RangeDeletionRecoveryTracker, NotifyJobForCompletedTermDoesNothing) {
    auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    endTerm(tracker, term);
    tracker.notifyRecoveryJobComplete(term);
    ASSERT_EQ(tracker.getTrackedTermsCount(), 0);
}

TEST(RangeDeletionRecoveryTracker, RecoveryFutureForEndedTermIsComplete) {
    auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    endTerm(tracker, term);
    auto future = tracker.getRecoveryFuture(term);
    ASSERT_TRUE(future.isReady());
    ASSERT_EQ(future.get(), RangeDeletionRecoveryTracker::Outcome::kUnknown);
}

TEST(RangeDeletionRecoveryTracker, EndTermAfterAllJobsComplete) {
    auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    tracker.registerRecoveryJob(term);
    auto future = tracker.getRecoveryFuture(term);
    tracker.notifyRecoveryJobComplete(term);
    endTerm(tracker, term);
    ASSERT_TRUE(future.isReady());
    ASSERT_EQ(future.get(), RangeDeletionRecoveryTracker::Outcome::kComplete);
}

DEATH_TEST(RangeDeletionRecoveryTracker,
           RegisterJobAfterRecoveryCompleteAsserts,
           "Tripwire assertion") {
    const auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    tracker.registerRecoveryJob(term);
    tracker.notifyRecoveryJobComplete(term);
    tracker.registerRecoveryJob(term);
}

}  // namespace mongo
