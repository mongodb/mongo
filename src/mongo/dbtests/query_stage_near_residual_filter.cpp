// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Tests for the residual predicate that the query planner pushes into the near stage in place of a
 * FETCH stage above it. See pushResidualFilterIntoGeoNear() in planner_analysis.cpp.
 */

#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/dbtests/query_stage_near_test_helpers.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

class QueryStageNearResidualFilterTest : public QueryStageNearTest {
protected:
    /**
     * Parses 'filter' into a MatchExpression. The parsed expression keeps BSONElements pointing
     * into 'filter', so the fixture holds on to a copy for the lifetime of the test.
     */
    std::unique_ptr<MatchExpression> parseFilter(const BSONObj& filter) {
        _filterOwnership.push_back(filter.getOwned());
        return MatchExpressionParser::parseAndNormalize(_filterOwnership.back(), _expCtx);
    }

    /**
     * A predicate that throws when it is evaluated on a document whose "divisor" is 0. This stands
     * in for a real predicate such as an $expr with a failing $convert: the point is that a
     * document which the near search discards must never reach the predicate.
     */
    std::unique_ptr<MatchExpression> makeThrowingFilter() {
        return parseFilter(fromjson("{$expr: {$eq: [{$divide: [1, '$divisor']}, 1]}}"));
    }

    const NearStats* getNearStats(const std::unique_ptr<PlanStageStats>& stats) {
        return static_cast<const NearStats*>(stats->specific.get());
    }

    std::vector<BSONObj> _filterOwnership;
};

TEST_F(QueryStageNearResidualFilterTest, ResidualFilterExcludesNonMatchingDocuments) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setResidualFilter(parseFilter(fromjson("{keep: true}")));

    nearStage.addInterval({BSON("distance" << 0.5 << "keep" << true),
                           BSON("distance" << 0.75 << "keep" << false),
                           BSON("distance" << 0.25)},
                          0.0,
                          1.0);
    nearStage.addInterval(
        {BSON("distance" << 1.5 << "keep" << false), BSON("distance" << 1.25 << "keep" << true)},
        1.0,
        2.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 2u);
    assertAscendingAndValid(results);
    for (const auto& result : results) {
        ASSERT(result["keep"].trueValue());
    }
}

TEST_F(QueryStageNearResidualFilterTest, ResidualFilterNotEvaluatedBelowIntervalMinDistance) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setResidualFilter(makeThrowingFilter());

    // The first document is closer than the interval's minimum distance, so the near search
    // discards it before the predicate is applied. Were the predicate applied first, the whole
    // query would fail.
    nearStage.addInterval(
        {BSON("distance" << 0.0 << "divisor" << 0), BSON("distance" << 1.5 << "divisor" << 1)},
        1.0,
        2.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 1u);
    ASSERT_EQUALS(results[0]["distance"].numberDouble(), 1.5);
}

TEST_F(QueryStageNearResidualFilterTest, ResidualFilterNotEvaluatedBeyondMaxSearchDistance) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setResidualFilter(makeThrowingFilter());
    nearStage.setMaxSearchDistance(2.0);

    // The covering of an interval may return documents beyond the interval, which are normally
    // buffered for a later interval. A document beyond the largest distance of the whole search can
    // never be returned though, so it is discarded before the predicate is applied. Were the
    // predicate applied to it, the whole query would fail.
    nearStage.addInterval(
        {BSON("distance" << 0.5 << "divisor" << 1), BSON("distance" << 3.0 << "divisor" << 0)},
        0.0,
        1.0);
    nearStage.addInterval({BSON("distance" << 1.5 << "divisor" << 1)}, 1.0, 2.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 2u);
    assertAscendingAndValid(results);
    ASSERT_EQUALS(results[0]["distance"].numberDouble(), 0.5);
    ASSERT_EQUALS(results[1]["distance"].numberDouble(), 1.5);
}

TEST_F(QueryStageNearResidualFilterTest, DocumentAtMaxSearchDistanceIsStillReturned) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setMaxSearchDistance(2.0);

    // The largest distance of the search is inclusive.
    nearStage.addInterval({BSON("distance" << 2.0)}, 0.0, 2.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 1u);
    ASSERT_EQUALS(results[0]["distance"].numberDouble(), 2.0);
}

TEST_F(QueryStageNearResidualFilterTest, ResidualFilterThrowsOnBufferedDocument) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setResidualFilter(makeThrowingFilter());

    // This document is inside the interval, so the predicate does get applied to it and its error
    // must surface.
    nearStage.addInterval({BSON("distance" << 1.5 << "divisor" << 0)}, 1.0, 2.0);

    ASSERT_THROWS(advanceStage(&nearStage, &workingSet), DBException);
}

TEST_F(QueryStageNearResidualFilterTest, RejectedDocumentsAreNotBuffered) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setResidualFilter(parseFilter(fromjson("{keep: true}")));

    nearStage.addInterval({BSON("distance" << 0.5 << "keep" << true),
                           BSON("distance" << 0.75 << "keep" << false),
                           BSON("distance" << 0.25 << "keep" << false)},
                          0.0,
                          1.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 1u);

    // A document rejected by the residual predicate must not be counted as buffered or returned,
    // since it never enters the distance sorter.
    const auto stats = nearStage.getStats();
    const auto* nearStats = getNearStats(stats);
    ASSERT_EQUALS(nearStats->intervalStats.size(), 1u);
    ASSERT_EQUALS(nearStats->intervalStats[0].numResultsBuffered, 1);
    ASSERT_EQUALS(nearStats->intervalStats[0].numResultsReturned, 1);
}

TEST_F(QueryStageNearResidualFilterTest, RejectedDocumentIsStillDeduplicated) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setResidualFilter(parseFilter(fromjson("{keep: true}")));

    // The same RecordId shows up in both intervals. The document is rejected by the predicate in
    // the first interval, but it must still be recorded as seen, so the second occurrence is
    // deduplicated away rather than being returned. This matches the behavior of the equivalent
    // plan with a FETCH stage above the near stage.
    nearStage.addInterval({BSON("rid" << 7 << "distance" << 0.5 << "keep" << false)}, 0.0, 1.0);
    nearStage.addInterval({BSON("rid" << 7 << "distance" << 1.5 << "keep" << true)}, 1.0, 2.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 0u);
}

TEST_F(QueryStageNearResidualFilterTest, AllDocumentsRejectedStillReachesEOF) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    nearStage.setResidualFilter(parseFilter(fromjson("{keep: true}")));

    for (int distance = 0; distance < 5; ++distance) {
        nearStage.addInterval(
            {BSON("distance" << distance << "keep" << false)}, distance, distance + 1);
    }

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 0u);
    ASSERT(nearStage.isEOF());
}

TEST_F(QueryStageNearResidualFilterTest, ResidualFilterIsReportedInStats) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    auto filter = parseFilter(fromjson("{keep: true}"));
    const BSONObj expectedFilter = filter->serialize();
    nearStage.setResidualFilter(std::move(filter));

    nearStage.addInterval({BSON("distance" << 0.5 << "keep" << true)}, 0.0, 1.0);
    advanceStage(&nearStage, &workingSet);

    // Explain reports the pushed-down predicate as the stage's filter, the same way a FETCH stage
    // above the near stage used to report it.
    const auto stats = nearStage.getStats();
    ASSERT_BSONOBJ_EQ(stats->common.filter, expectedFilter);
}

TEST_F(QueryStageNearResidualFilterTest, ResidualFilterIsReportedInStatsEvenWhenNoDocMatches) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);
    auto filter = parseFilter(fromjson("{keep: true}"));
    const BSONObj expectedFilter = filter->serialize();
    nearStage.setResidualFilter(std::move(filter));

    nearStage.addInterval({BSON("distance" << 0.5 << "keep" << false)}, 0.0, 1.0);
    advanceStage(&nearStage, &workingSet);

    // The pushed-down predicate must still be visible in explain output even if every document is
    // rejected by it.
    const auto stats = nearStage.getStats();
    ASSERT_BSONOBJ_EQ(stats->common.filter, expectedFilter);
}

TEST_F(QueryStageNearResidualFilterTest, NoResidualFilterMeansNoFilterInStats) {
    WorkingSet workingSet;
    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);

    nearStage.addInterval({BSON("distance" << 0.5)}, 0.0, 1.0);
    advanceStage(&nearStage, &workingSet);

    const auto stats = nearStage.getStats();
    ASSERT_BSONOBJ_EQ(stats->common.filter, BSONObj());
}

}  // namespace
}  // namespace mongo
