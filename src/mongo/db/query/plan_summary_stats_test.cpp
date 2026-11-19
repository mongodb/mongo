// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_summary_stats.h"

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using SpillingStage = PlanSummaryStats::SpillingStage;

SpillingStats makeStats(uint64_t spills, uint64_t bytes, uint64_t records, uint64_t storageSize) {
    SpillingStats stats;
    stats.updateSpillingStats(spills, bytes, records, storageSize);
    return stats;
}

TEST(PlanSummaryStatsTest, TotalSpillingStatsIsZeroWhenNothingSpilled) {
    PlanSummaryStats stats;
    auto total = stats.getTotalSpillingStats();
    ASSERT_EQ(total.getSpills(), 0u);
    ASSERT_EQ(total.getSpilledBytes(), 0u);
    ASSERT_EQ(total.getSpilledRecords(), 0u);
    ASSERT_EQ(total.getSpilledDataStorageSize(), 0u);
}

TEST(PlanSummaryStatsTest, TotalSpillingStatsMatchesSingleStage) {
    PlanSummaryStats stats;
    stats.spillingStatsPerStage[SpillingStage::SORT] = makeStats(2, 100, 10, 40);

    auto total = stats.getTotalSpillingStats();
    ASSERT_EQ(total.getSpills(), 2u);
    ASSERT_EQ(total.getSpilledBytes(), 100u);
    ASSERT_EQ(total.getSpilledRecords(), 10u);
    ASSERT_EQ(total.getSpilledDataStorageSize(), 40u);
}

TEST(PlanSummaryStatsTest, TotalSpillingStatsSumsAcrossStages) {
    PlanSummaryStats stats;
    stats.spillingStatsPerStage[SpillingStage::SORT] = makeStats(2, 100, 10, 40);
    stats.spillingStatsPerStage[SpillingStage::GROUP] = makeStats(3, 200, 20, 60);
    stats.spillingStatsPerStage[SpillingStage::SET_WINDOW_FIELDS] = makeStats(1, 50, 5, 5);

    auto total = stats.getTotalSpillingStats();
    ASSERT_EQ(total.getSpills(), 6u);
    ASSERT_EQ(total.getSpilledBytes(), 350u);
    ASSERT_EQ(total.getSpilledRecords(), 35u);
    ASSERT_EQ(total.getSpilledDataStorageSize(), 105u);
}

TEST(PlanSummaryStatsTest, TotalSpillingStatsDoesNotModifyPerStageStats) {
    PlanSummaryStats stats;
    stats.spillingStatsPerStage[SpillingStage::SORT] = makeStats(2, 100, 10, 40);
    stats.spillingStatsPerStage[SpillingStage::GROUP] = makeStats(3, 200, 20, 60);

    (void)stats.getTotalSpillingStats();
    (void)stats.getTotalSpillingStats();

    ASSERT_EQ(stats.spillingStatsPerStage.at(SpillingStage::SORT).getSpills(), 2u);
    ASSERT_EQ(stats.spillingStatsPerStage.at(SpillingStage::GROUP).getSpilledBytes(), 200u);
}

TEST(PlanSummaryStatsTest, VisitorCountsSpoolSpills) {
    SpoolStats spoolStats;
    spoolStats.spillingStats.updateSpillingStats(2, 100, 10, 40);

    PlanSummaryStats summary;
    PlanSummaryStatsVisitor visitor(summary);
    spoolStats.acceptVisitor(&visitor);

    ASSERT_TRUE(summary.usedDisk);
    const auto& spool = summary.spillingStatsPerStage.at(SpillingStage::SPOOL);
    ASSERT_EQ(spool.getSpills(), 2u);
    ASSERT_EQ(spool.getSpilledBytes(), 100u);
    ASSERT_EQ(spool.getSpilledRecords(), 10u);
    ASSERT_EQ(spool.getSpilledDataStorageSize(), 40u);
    // The spool's spills are part of the query-wide totals.
    ASSERT_EQ(summary.getTotalSpillingStats().getSpills(), 2u);
}

TEST(PlanSummaryStatsTest, VisitorIgnoresSpoolThatDidNotSpill) {
    SpoolStats spoolStats;

    PlanSummaryStats summary;
    PlanSummaryStatsVisitor visitor(summary);
    spoolStats.acceptVisitor(&visitor);

    ASSERT_FALSE(summary.usedDisk);
    ASSERT_TRUE(summary.spillingStatsPerStage.empty());
}

}  // namespace
}  // namespace mongo
