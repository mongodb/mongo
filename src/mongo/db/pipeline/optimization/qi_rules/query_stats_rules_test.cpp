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
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class QueryStatsMatchPushdownOptimizationRulesTest : public AggregationContextFixture {
public:
    QueryStatsMatchPushdownOptimizationRulesTest()
        : AggregationContextFixture(
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin)),
          _enableMatchPushdownOptimization("internalQueryStatsMatchPushdownOptimizationEnabled",
                                           true) {}

protected:
    // Runs pipeline optimization over 'stagesJson' (the first stage must be $queryStats) and
    // returns the keyHash lookup locations the $queryStats stage captured. The value selects the
    // execution path the stage takes:
    //   boost::none               = full store scan
    //   engaged, empty vector     = short-circuit, no scan
    //   engaged, non-empty vector = index scan for exactly those keyHash values
    boost::optional<std::vector<std::string>> pushdownDecisionFor(
        const std::vector<std::string>& stagesJson) {
        std::vector<BSONObj> bsonStages;
        for (auto&& stage : stagesJson) {
            bsonStages.push_back(fromjson(stage));
        }
        auto pipeline = pipeline_factory::makePipeline(
            bsonStages, getExpCtx(), pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);
        auto* queryStatsStage =
            dynamic_cast<DocumentSourceQueryStats*>(pipeline->getSources().front().get());
        ASSERT(queryStatsStage) << "the first stage is not $queryStats";
        return queryStatsStage->getKeyHashLookupLocations_forTest();
    }

    // The decision for the common [$queryStats, $match(filter)] pipeline.
    boost::optional<std::vector<std::string>> decisionForMatch(const BSONObj& filter) {
        return pushdownDecisionFor({R"({$queryStats: {}})", BSON("$match" << filter).jsonString()});
    }

private:
    // Restores the knob to its pre-suite value on fixture teardown.
    unittest::ServerParameterGuard _enableMatchPushdownOptimization;
};

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KeyHashEqEngagesLookup) {
    ASSERT_EQ(decisionForMatch(fromjson("{keyHash: 'X'}")), (std::vector<std::string>{"X"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KeyHashInEngagesLookup) {
    ASSERT_EQ(decisionForMatch(fromjson("{keyHash: {$in: ['X', 'Y']}}")),
              (std::vector<std::string>{"X", "Y"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, EmptyInShortCircuits) {
    // An empty $in can match nothing, so the stage short-circuits without scanning.
    auto decision = decisionForMatch(fromjson("{keyHash: {$in: []}}"));
    ASSERT_TRUE(decision.has_value());
    ASSERT_TRUE(decision->empty());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KeyHashConjunctsCapturedWithOtherPredicates) {
    // The keyHash conjuncts are captured; the residual $match filters the rest.
    ASSERT_EQ(decisionForMatch(fromjson("{keyHash: 'X', a: 1}")), (std::vector<std::string>{"X"}));
    ASSERT_EQ(decisionForMatch(fromjson("{$and: [{keyHash: 'X'}, {a: 1}]}")),
              (std::vector<std::string>{"X"}));
    ASSERT_EQ(decisionForMatch(fromjson("{keyHash: {$in: ['X', 'Y']}, a: 1}")),
              (std::vector<std::string>{"X", "Y"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KeyHashConjunctsCapturedThroughNestedAnd) {
    ASSERT_EQ(decisionForMatch(fromjson("{$and: [{$and: [{keyHash: 'X'}, {b: 2}]}, {c: 3}]}")),
              (std::vector<std::string>{"X"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, DisjointKeyHashConjunctsShortCircuit) {
    // A document has one keyHash, so requiring two different ones makes the $and unsatisfiable.
    auto decision = decisionForMatch(fromjson("{$and: [{keyHash: 'X'}, {keyHash: 'Y'}]}"));
    ASSERT_TRUE(decision.has_value());
    ASSERT_TRUE(decision->empty());

    decision = decisionForMatch(fromjson("{$and: [{keyHash: 'X'}, {keyHash: {$in: ['Y']}}]}"));
    ASSERT_TRUE(decision.has_value());
    ASSERT_TRUE(decision->empty());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, OverlappingKeyHashConjunctsIntersect) {
    // Not merely "two keyHash conjuncts means empty": the conjuncts intersect.
    ASSERT_EQ(decisionForMatch(fromjson("{$and: [{keyHash: 'X'}, {keyHash: {$in: ['X', 'Y']}}]}")),
              (std::vector<std::string>{"X"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KeyHashNotCapturedThroughUnnarrowableOrOrNor) {
    // An $or branch that is not a keyHash equality admits any keyHash, so the $or cannot narrow.
    ASSERT_FALSE(decisionForMatch(fromjson("{$and: [{$or: [{keyHash: 'X'}, {a: 1}]}, {b: 2}]}"))
                     .has_value());
    ASSERT_FALSE(
        decisionForMatch(fromjson("{$and: [{$nor: [{keyHash: 'X'}]}, {b: 2}]}")).has_value());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, NotPushedDownForOrWithNonKeyHashBranch) {
    ASSERT_FALSE(decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {a: 1}]}")).has_value());
    ASSERT_FALSE(decisionForMatch(fromjson("{a: 1}")).has_value());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, NonStringKeyHashOperandMatchesNothing) {
    // 'keyHash' is always a string on the stage's output, so a non-string operand can never match.
    auto decision = decisionForMatch(fromjson("{keyHash: 5}"));
    ASSERT_TRUE(decision.has_value());
    ASSERT_TRUE(decision->empty());

    // Such a branch drops out of an $or, and zeroes an $and.
    ASSERT_EQ(decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {keyHash: 5}]}")),
              (std::vector<std::string>{"X"}));
    decision = decisionForMatch(fromjson("{$and: [{keyHash: 'X'}, {keyHash: 5}]}"));
    ASSERT_TRUE(decision.has_value());
    ASSERT_TRUE(decision->empty());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, NonStringInElementsAreDropped) {
    ASSERT_EQ(decisionForMatch(fromjson("{keyHash: {$in: ['X', 5]}}")),
              (std::vector<std::string>{"X"}));

    auto decision = decisionForMatch(fromjson("{keyHash: {$in: [5]}}"));
    ASSERT_TRUE(decision.has_value());
    ASSERT_TRUE(decision->empty());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KeyHashDisjunctsCapturedThroughOr) {
    ASSERT_EQ(decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {keyHash: 'Y'}]}")),
              (std::vector<std::string>{"X", "Y"}));
    ASSERT_EQ(decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {keyHash: {$in: ['Y', 'Z']}}]}")),
              (std::vector<std::string>{"X", "Y", "Z"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KeyHashDisjunctsCapturedThroughNestedOr) {
    ASSERT_EQ(decisionForMatch(fromjson("{$or: [{$or: [{keyHash: 'X'}]}, {keyHash: 'Y'}]}")),
              (std::vector<std::string>{"X", "Y"}));
    // An $or nested in an $and still narrows; the other conjunct is skipped as usual.
    ASSERT_EQ(
        decisionForMatch(fromjson("{$and: [{$or: [{keyHash: 'X'}, {keyHash: 'Y'}]}, {a: 1}]}")),
        (std::vector<std::string>{"X", "Y"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, CompoundDisjunctsCapturedThroughOr) {
    // Each branch carries its own predicates, so this has no $in equivalent.
    ASSERT_EQ(decisionForMatch(fromjson("{$or: [{keyHash: 'X', a: 1}, {keyHash: 'Y'}]}")),
              (std::vector<std::string>{"X", "Y"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, UnsatisfiableOrBranchContributesNothing) {
    // A branch that matches nothing drops out of the union rather than zeroing it, unlike $and.
    ASSERT_EQ(decisionForMatch(fromjson("{$or: [{keyHash: {$in: []}}, {keyHash: 'X'}]}")),
              (std::vector<std::string>{"X"}));

    // Every branch matches nothing, so the stage short-circuits without scanning.
    auto decision = decisionForMatch(fromjson("{$or: [{keyHash: {$in: []}}]}"));
    ASSERT_TRUE(decision.has_value());
    ASSERT_TRUE(decision->empty());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, NotPushedDownForUnnarrowableOrBranch) {
    ASSERT_FALSE(
        decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {keyHash: {$ne: 'Y'}}]}")).has_value());
    ASSERT_FALSE(decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {keyHash: {$in: [/re/]}}]}"))
                     .has_value());
    ASSERT_FALSE(decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {$nor: [{keyHash: 'Y'}]}]}"))
                     .has_value());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, FallsBackUnderNonSimpleCollation) {
    getExpCtx()->setCollator(
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString));

    ASSERT_FALSE(decisionForMatch(fromjson("{keyHash: 'X'}")).has_value());
    ASSERT_FALSE(decisionForMatch(fromjson("{keyHash: {$in: ['X', 'Y']}}")).has_value());
    ASSERT_FALSE(decisionForMatch(fromjson("{keyHash: 'X', a: 1}")).has_value());
    ASSERT_FALSE(decisionForMatch(fromjson("{$or: [{keyHash: 'X'}, {keyHash: 'Y'}]}")).has_value());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, CapturesAdjacentMatchWithMoreStages) {
    // The keyHash conjuncts come from the $match immediately after $queryStats; later stages do not
    // affect the decision.
    ASSERT_EQ(pushdownDecisionFor(
                  {R"({$queryStats: {}})", R"({$match: {keyHash: 'X'}})", R"({$match: {a: 1}})"}),
              (std::vector<std::string>{"X"}));
    ASSERT_EQ(
        pushdownDecisionFor(
            {R"({$queryStats: {}})", R"({$match: {keyHash: 'X'}})", R"({$project: {keyHash: 1}})"}),
        (std::vector<std::string>{"X"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, CapturesMatchHoistedAboveProject) {
    // The reordering pass hoists the $match above a $project that leaves keyHash alone, so the
    // capture still sees an adjacent $match.
    ASSERT_EQ(
        pushdownDecisionFor(
            {R"({$queryStats: {}})", R"({$project: {keyHash: 1}})", R"({$match: {keyHash: 'X'}})"}),
        (std::vector<std::string>{"X"}));

    // A $project that recomputes keyHash cannot be swapped with, so the $match stays below it and
    // the capture never sees it.
    ASSERT_FALSE(pushdownDecisionFor({R"({$queryStats: {}})",
                                      R"({$project: {keyHash: {$literal: 'X'}}})",
                                      R"({$match: {keyHash: 'X'}})"})
                     .has_value());
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, CapturesSplitMatchHoistedAboveProject) {
    // The $project modifies 'a' but leaves keyHash alone, so only the keyHash conjunct is hoisted
    // above it. That hoisted $match is what the capture sees; the 'a' conjunct stays below.
    ASSERT_EQ(pushdownDecisionFor({R"({$queryStats: {}})",
                                   R"({$project: {keyHash: 1, a: {$literal: 1}}})",
                                   R"({$match: {keyHash: 'X', a: 1}})"}),
              (std::vector<std::string>{"X"}));
}

TEST_F(QueryStatsMatchPushdownOptimizationRulesTest, KnobDisabledDoesNotCapture) {
    unittest::ServerParameterGuard disableKnob("internalQueryStatsMatchPushdownOptimizationEnabled",
                                               false);
    ASSERT_FALSE(decisionForMatch(fromjson("{keyHash: 'X'}")).has_value());
    ASSERT_FALSE(decisionForMatch(fromjson("{keyHash: {$in: ['X', 'Y']}}")).has_value());
}

/**
 * Unit tests for the '$queryStats' top-K sort optimization rule, which annotates the $queryStats
 * stage with a TopKSortSpec hint when it is followed by [optional $project] -> $sort(absorbed
 * $limit) over a supported metric.
 */
class QueryStatsTopKOptimizationRulesTest : public AggregationContextFixture {
public:
    QueryStatsTopKOptimizationRulesTest()
        : AggregationContextFixture(
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin)) {}

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
