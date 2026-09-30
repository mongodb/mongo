// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/query_stats_top_k_metrics.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

/**
 * Unit tests for the '$queryStats' top-K sort optimization rule, which annotates the $queryStats
 * stage with a TopKSortSpec hint when it is followed by [optional $project] -> $sort(absorbed
 * $limit) over a supported metric.
 */
class QueryStatsTopKOptimizationRulesTest : public AggregationContextFixture {
public:
    QueryStatsTopKOptimizationRulesTest()
        : AggregationContextFixture(
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin)),
          // TODO SERVER-135111: remove once the optimization is enabled by default.
          _enableTopKSortOptimization("internalQueryStatsTopKSortOptimizationEnabled", true) {}

protected:
    struct ExpectedTopKSpec {
        std::string path;
        long long limit;
        bool isAscending;
    };

    std::unique_ptr<Pipeline> parseAndOptimize(std::vector<std::string> stagesJson) {
        std::vector<BSONObj> bsonStages;
        for (auto&& stage : stagesJson) {
            bsonStages.push_back(fromjson(stage));
        }
        auto pipeline = pipeline_factory::makePipeline(
            bsonStages, getExpCtx(), pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);
        return pipeline;
    }

    boost::optional<BSONObj> getTopKSortOptimizationSpec(const Pipeline& pipeline) {
        ASSERT(!pipeline.getSources().empty());
        auto* queryStatsStage =
            dynamic_cast<DocumentSourceQueryStats*>(pipeline.getSources().front().get());
        ASSERT(queryStatsStage) << "the first stage is not $queryStats";

        query_shape::SerializationOptions explainOpts;
        explainOpts.verbosity = ExplainOptions::Verbosity::kQueryPlanner;
        Value topKSort = queryStatsStage->serialize(explainOpts)
                             .getDocument()["$queryStats"]["topKSortOptimization"];
        if (topKSort.missing()) {
            return boost::none;
        }
        return topKSort.getDocument().toBson();
    }

    void assertOptimizationApplies(const std::vector<std::string>& stagesJson,
                                   const ExpectedTopKSpec& expected) {
        auto pipeline = parseAndOptimize(stagesJson);
        auto spec = getTopKSortOptimizationSpec(*pipeline);
        ASSERT(spec) << "expected the optimization to occur";
        ASSERT_BSONOBJ_EQ(BSON("path" << expected.path << "limit" << expected.limit << "isAscending"
                                      << expected.isAscending),
                          *spec);
    }

    void assertNoOptimization(const std::vector<std::string>& stagesJson) {
        auto pipeline = parseAndOptimize(stagesJson);
        ASSERT_FALSE(getTopKSortOptimizationSpec(*pipeline).has_value());
    }

private:
    // Restores the knob to its pre-suite value on fixture teardown.
    unittest::ServerParameterGuard _enableTopKSortOptimization;
};

TEST_F(QueryStatsTopKOptimizationRulesTest, AppliesOnMinimalPipelineWithoutProjection) {
    assertOptimizationApplies(
        {R"({$queryStats: {}})", R"({$sort: {"metrics.execCount": -1}})", R"({$limit: 7})"},
        {.path = "metrics.execCount", .limit = 7, .isAscending = false});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, AppliesOnAscendingSortWithProjection) {
    assertOptimizationApplies(
        {R"({$queryStats: {}})",
         R"({$project: {keyHash: 1, "metrics.workingTimeMillis": 1}})",
         R"({$sort: {"metrics.workingTimeMillis.min": 1}})",
         R"({$limit: 3})"},
        {.path = "metrics.workingTimeMillis.min", .limit = 3, .isAscending = true});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, AppliesWhenProjectionPreservesExactSortPath) {
    assertOptimizationApplies(
        {R"({$queryStats: {}})",
         R"({$project: {keyHash: 1, "metrics.workingTimeMillis.min": -1}})",
         R"({$sort: {"metrics.workingTimeMillis.min": -1}})",
         R"({$limit: 5})"},
        {.path = "metrics.workingTimeMillis.min", .limit = 5, .isAscending = false});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, AppliesOnSurvivingSortWhenEarlierSortPresent) {
    // Two adjacent $sorts collapse into the last one before this rule runs, so the surviving sort
    // is what gets captured here.
    assertOptimizationApplies({R"({$queryStats: {}})",
                               R"({$sort: {"metrics.workingTimeMillis.min": 1}})",
                               R"({$sort: {"metrics.execCount": -1}})",
                               R"({$limit: 5})"},
                              {.path = "metrics.execCount", .limit = 5, .isAscending = false});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, AppliesWhenLimitEqualsTheMaximum) {
    assertOptimizationApplies(
        {R"({$queryStats: {}})",
         R"({$sort: {"metrics.execCount": -1}})",
         R"({$limit: )" + std::to_string(query_stats::kTopKOptimizationMaxLimit) + R"(})"},
        {.path = "metrics.execCount",
         .limit = query_stats::kTopKOptimizationMaxLimit,
         .isAscending = false});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyWhenLimitExceedsTheMaximum) {
    assertNoOptimization(
        {R"({$queryStats: {}})",
         R"({$sort: {"metrics.execCount": -1}})",
         R"({$limit: )" + std::to_string(query_stats::kTopKOptimizationMaxLimit + 1) + R"(})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOnUnsupportedMetric) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$sort: {"metrics.queryExec.readTimeMicros": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyWithoutLimit) {
    assertNoOptimization({R"({$queryStats: {}})", R"({$sort: {"metrics.execCount": -1}})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyWithoutSort) {
    assertNoOptimization({R"({$queryStats: {}})", R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOnCompoundSortKey) {
    assertNoOptimization(
        {R"({$queryStats: {}})",
         R"({$sort: {"metrics.execCount": -1, "metrics.workingTimeMillis.max": 1}})",
         R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyWithInterveningMatch) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$match: {keyHash: {$in: ["fakeHash"]}}})",
                          R"({$sort: {"metrics.execCount": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyWhenProjectionDoesNotPreserveSortField) {
    // The inclusion-only projection preserves 'metrics.workingTimeMillis.sum' but not '.min', so
    // the $sort below may be ordering on a path the projection drops.
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$project: {keyHash: 1, "metrics.workingTimeMillis.sum": 1}})",
                          R"({$sort: {"metrics.workingTimeMillis.min": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOverComputedProjection) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$project: {newField: {$add: ["$metrics.execCount", 1]}}})",
                          R"({$sort: {"metrics.execCount": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyWhenProjectionRenamesSortField) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$project: {"metrics.execCount": "$metrics.workingTimeMillis.sum"}})",
                          R"({$sort: {"metrics.execCount": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOverExclusionProjection) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$project: {"metrics.workingTimeMillis.sum": 0}})",
                          R"({$sort: {"metrics.workingTimeMillis.sum": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOverSetStage) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$set: {newField: "$metrics.execCount"}})",
                          R"({$sort: {"metrics.execCount": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOverAddFieldsStage) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$addFields: {newField: 1}})",
                          R"({$sort: {"metrics.execCount": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOverReplaceRootStage) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$replaceRoot: {newRoot: "$metrics"}})",
                          R"({$sort: {"metrics.execCount": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyOverUnsetStage) {
    assertNoOptimization({R"({$queryStats: {}})",
                          R"({$unset: "metrics.workingTimeMillis.sum"})",
                          R"({$sort: {"metrics.workingTimeMillis.sum": -1}})",
                          R"({$limit: 5})"});
}

TEST_F(QueryStatsTopKOptimizationRulesTest, DoesNotApplyWhenKnobDisabled) {
    unittest::ServerParameterGuard guard("internalQueryStatsTopKSortOptimizationEnabled", false);
    assertNoOptimization(
        {R"({$queryStats: {}})", R"({$sort: {"metrics.execCount": -1}})", R"({$limit: 5})"});
}

}  // namespace
}  // namespace mongo
