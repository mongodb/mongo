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

#include "mongo/db/curop_metrics.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"


namespace mongo {
namespace {

class CurOpMetricsTest : public ServiceContextTest {
protected:
    CurOpMetricsTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
                  nullptr, nullptr, std::make_unique<TickSourceMock<Milliseconds>>()))) {}

    TickSourceMock<Milliseconds>* tickSource() {
        return checked_cast<TickSourceMock<Milliseconds>*>(getServiceContext()->getTickSource());
    }

    void advance(Milliseconds ms) {
        tickSource()->advance(ms);
    }
};

TEST_F(CurOpMetricsTest, TrackerIsDefaultInitialized) {
    auto opCtx = makeOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx.get());
    ASSERT_FALSE(tracker.hasIntervalStart);
    ASSERT_FALSE(tracker.hadLongInterval);
    ASSERT_EQ(tracker.longIntervalCount, 0);
    ASSERT_EQ(tracker.longIntervalTotalMs, 0);
    ASSERT_EQ(tracker.intervalStartTick, 0);
}

TEST_F(CurOpMetricsTest, TrackerIsIndependentPerOpCtx) {
    // Verify the tracker on one opCtx doesn't affect a subsequent opCtx.
    {
        auto opCtx1 = makeOperationContext();
        auto& tracker1 = getAggNonTicketedIntervalTracker(opCtx1.get());
        tracker1.hasIntervalStart = true;
        tracker1.hadLongInterval = true;
        tracker1.longIntervalCount = 5;
    }
    // After opCtx1 is destroyed, a new opCtx should start with a fresh tracker.
    auto opCtx2 = makeOperationContext();
    auto& tracker2 = getAggNonTicketedIntervalTracker(opCtx2.get());
    ASSERT_FALSE(tracker2.hasIntervalStart);
    ASSERT_FALSE(tracker2.hadLongInterval);
    ASSERT_EQ(tracker2.longIntervalCount, 0);
}

TEST_F(CurOpMetricsTest, ThresholdReturnsPositiveValue) {
    // The threshold must be a valid positive millisecond count.
    ASSERT_GT(aggNonTicketedIntervalThresholdMillis(), 0);
}

TEST_F(CurOpMetricsTest, SimulatedIntervalBelowThresholdNotCounted) {
    auto opCtx = makeOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx.get());
    auto threshold = aggNonTicketedIntervalThresholdMillis();

    tracker.openInterval(opCtx->tickSource().getTicks());
    advance(Milliseconds(threshold - 1));
    auto& ts = opCtx->tickSource();
    tracker.closeInterval(
        ts.ticksTo<Milliseconds>(ts.getTicks() - tracker.intervalStartTick).count(), threshold);

    ASSERT_FALSE(tracker.hadLongInterval);
    ASSERT_EQ(tracker.longIntervalCount, 0);
    ASSERT_EQ(tracker.longIntervalTotalMs, 0);
}

TEST_F(CurOpMetricsTest, SimulatedIntervalAtOrAboveThresholdCounted) {
    auto opCtx = makeOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx.get());
    auto threshold = aggNonTicketedIntervalThresholdMillis();

    tracker.openInterval(opCtx->tickSource().getTicks());
    advance(Milliseconds(threshold + 10));
    auto& ts = opCtx->tickSource();
    tracker.closeInterval(
        ts.ticksTo<Milliseconds>(ts.getTicks() - tracker.intervalStartTick).count(), threshold);

    ASSERT_TRUE(tracker.hadLongInterval);
    ASSERT_EQ(tracker.longIntervalCount, 1);
    ASSERT_GTE(tracker.longIntervalTotalMs, threshold);
    ASSERT_GTE(tracker.longIntervalMaxMs, threshold);
}

// Tests for closeAggNonTicketedIntervalIfOpen(), the shared function called by both
// DSCatalogResourceHandleBase::acquire() (to close any still-open interval when the ticket is
// re-acquired mid-command) and _flushAggNonTicketedStats() (to close an open interval at
// command end before flushing to global counters).

TEST_F(CurOpMetricsTest, CloseIfOpenIsNoOpWhenClosed) {
    auto opCtx = makeOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx.get());

    // No interval has been opened; calling the function should change nothing.
    closeAggNonTicketedIntervalIfOpen(tracker, opCtx.get());

    ASSERT_FALSE(tracker.hasIntervalStart);
    ASSERT_FALSE(tracker.hadLongInterval);
    ASSERT_EQ(tracker.longIntervalCount, 0);
}

TEST_F(CurOpMetricsTest, CloseIfOpenClosesAndRecordsLongInterval) {
    auto opCtx = makeOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx.get());
    auto threshold = aggNonTicketedIntervalThresholdMillis();

    tracker.openInterval(opCtx->tickSource().getTicks());
    advance(Milliseconds(threshold + 10));

    closeAggNonTicketedIntervalIfOpen(tracker, opCtx.get());

    ASSERT_FALSE(tracker.hasIntervalStart);
    ASSERT_TRUE(tracker.hadLongInterval);
    ASSERT_EQ(tracker.longIntervalCount, 1);
    ASSERT_GTE(tracker.longIntervalTotalMs, threshold);
}

TEST_F(CurOpMetricsTest, CloseIfOpenIsIdempotent) {
    // A second call with no open interval must not double-count the previous long interval.
    auto opCtx = makeOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx.get());
    auto threshold = aggNonTicketedIntervalThresholdMillis();

    tracker.openInterval(opCtx->tickSource().getTicks());
    advance(Milliseconds(threshold + 10));
    closeAggNonTicketedIntervalIfOpen(tracker, opCtx.get());
    advance(Milliseconds(threshold + 10));
    closeAggNonTicketedIntervalIfOpen(tracker, opCtx.get());  // no-op: interval already closed

    ASSERT_EQ(tracker.longIntervalCount, 1);
}

TEST_F(CurOpMetricsTest, MultipleIntervalsAccumulateCorrectly) {
    auto opCtx = makeOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx.get());
    auto threshold = aggNonTicketedIntervalThresholdMillis();

    auto closeInterval = [&]() {
        auto& ts = opCtx->tickSource();
        tracker.closeInterval(
            ts.ticksTo<Milliseconds>(ts.getTicks() - tracker.intervalStartTick).count(), threshold);
    };

    // First interval: above threshold.
    tracker.openInterval(opCtx->tickSource().getTicks());
    advance(Milliseconds(threshold + 5));
    closeInterval();

    // Second interval: below threshold.
    tracker.openInterval(opCtx->tickSource().getTicks());
    advance(Milliseconds(threshold - 1));
    closeInterval();

    // Third interval: above threshold.
    tracker.openInterval(opCtx->tickSource().getTicks());
    advance(Milliseconds(threshold + 20));
    closeInterval();

    ASSERT_TRUE(tracker.hadLongInterval);
    ASSERT_EQ(tracker.longIntervalCount, 2);
    ASSERT_GTE(tracker.longIntervalTotalMs, (threshold + 5) + (threshold + 20));
    // The max should be the larger of the two long intervals (threshold + 20, not threshold + 5).
    ASSERT_GTE(tracker.longIntervalMaxMs, threshold + 20);
}

}  // namespace
}  // namespace mongo
