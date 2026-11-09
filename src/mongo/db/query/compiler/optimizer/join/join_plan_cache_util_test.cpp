// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_plan_cache_util.h"

#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
namespace {

using InsertResult = JoinPlanCacheInsertResult;

constexpr size_t kThreshold = 100;

JoinPlanBaseline makeBaseline(size_t numReads, size_t numIntermediateRows) {
    return JoinPlanBaseline{.numReads = numReads, .numIntermediateRows = numIntermediateRows};
}

TEST(JoinPlanCacheInsertTest, InsertsPlanWithinThreshold) {
    ASSERT_EQ(InsertResult::kInsert,
              checkCanInsertIntoJoinPlanCache(
                  true, /*hitStashMemoryLimit=*/false, makeBaseline(10, 20), kThreshold));
}

TEST(JoinPlanCacheInsertTest, InsertsPlanExactlyAtThreshold) {
    ASSERT_EQ(InsertResult::kInsert,
              checkCanInsertIntoJoinPlanCache(
                  true, /*hitStashMemoryLimit=*/false, makeBaseline(60, 40), kThreshold));
}

TEST(JoinPlanCacheInsertTest, SkipsPlanOneOverThreshold) {
    ASSERT_EQ(InsertResult::kSkipOverThreshold,
              checkCanInsertIntoJoinPlanCache(
                  true, /*hitStashMemoryLimit=*/false, makeBaseline(60, 41), kThreshold));
}

TEST(JoinPlanCacheInsertTest, ThresholdSumsReadsAndIntermediateRows) {
    ASSERT_EQ(
        InsertResult::kSkipOverThreshold,
        checkCanInsertIntoJoinPlanCache(
            true, /*hitStashMemoryLimit=*/false, makeBaseline(0, kThreshold + 1), kThreshold));
    ASSERT_EQ(
        InsertResult::kSkipOverThreshold,
        checkCanInsertIntoJoinPlanCache(
            true, /*hitStashMemoryLimit=*/false, makeBaseline(kThreshold + 1, 0), kThreshold));
}

TEST(JoinPlanCacheInsertTest, CollectionsChangedTakesPrecedence) {
    ASSERT_EQ(InsertResult::kSkipCollectionsChanged,
              checkCanInsertIntoJoinPlanCache(false,
                                              /*hitStashMemoryLimit=*/true,
                                              makeBaseline(kThreshold, 1),
                                              kThreshold));
}

TEST(JoinPlanCacheInsertTest, StashLimitTakesPrecedenceOverThreshold) {
    ASSERT_EQ(InsertResult::kSkipOverStashMemoryLimit,
              checkCanInsertIntoJoinPlanCache(true,
                                              /*hitStashMemoryLimit=*/true,
                                              makeBaseline(kThreshold, 1),
                                              kThreshold));
}

TEST(JoinPlanCacheBaselineTest, BaselineReadsTrackerMetrics) {
    auto tracker = makeBaselineTracker();
    tracker->trackProgress<TrialRunTracker::kNumReads>(7);
    tracker->trackProgress<TrialRunTracker::kNumIntermediateRows>(5);

    const auto baseline = baselineFromTrial(*tracker, 3);
    ASSERT_EQ(baseline.numReads, 7u);
    ASSERT_EQ(baseline.numIntermediateRows, 5u);
    ASSERT_FALSE(baseline.spilled);
    ASSERT_EQ(baseline.numResults, 3u);
}

class CanUseJoinPlanCacheTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
    }

    const QueryKnobConfiguration& qkc() {
        return QueryKnobConfiguration::get(_opCtx.get());
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(CanUseJoinPlanCacheTest, UsedWhenEnabled) {
    QueryKnobGuardForTest enabled{_opCtx.get(), "internalEnableJoinPlanCache", true};
    ASSERT_TRUE(canUseJoinPlanCache(qkc(), /*hasHint=*/false, /*isExplain=*/false));
}

TEST_F(CanUseJoinPlanCacheTest, NotUsedWithHint) {
    QueryKnobGuardForTest enabled{_opCtx.get(), "internalEnableJoinPlanCache", true};
    ASSERT_FALSE(canUseJoinPlanCache(qkc(), /*hasHint=*/true, /*isExplain=*/false));
}

TEST_F(CanUseJoinPlanCacheTest, NotUsedForExplain) {
    QueryKnobGuardForTest enabled{_opCtx.get(), "internalEnableJoinPlanCache", true};
    ASSERT_FALSE(canUseJoinPlanCache(qkc(), /*hasHint=*/false, /*isExplain=*/true));
}

TEST_F(CanUseJoinPlanCacheTest, NotUsedInRandomReorderMode) {
    QueryKnobGuardForTest enabled{_opCtx.get(), "internalEnableJoinPlanCache", true};
    QueryKnobGuardForTest random{_opCtx.get(), "internalJoinReorderMode", "random"};
    ASSERT_FALSE(canUseJoinPlanCache(qkc(), /*hasHint=*/false, /*isExplain=*/false));
}

}  // namespace
}  // namespace mongo::join_ordering
