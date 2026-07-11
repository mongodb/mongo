// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

using RangeDeletionRecoveryTrackerDeathTest = RangeDeletionRecoveryTracker;
DEATH_TEST(RangeDeletionRecoveryTrackerDeathTest,
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

DEATH_TEST(RangeDeletionRecoveryTrackerDeathTest,
           RegisterJobAfterRecoveryCompleteAsserts,
           "Tripwire assertion") {
    const auto term = 0;
    RangeDeletionRecoveryTracker tracker;
    tracker.registerRecoveryJob(term);
    tracker.notifyRecoveryJobComplete(term);
    tracker.registerRecoveryJob(term);
}

}  // namespace mongo
