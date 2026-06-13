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

#include "mongo/db/query/engine_selection_plan.h"

#include "mongo/bson/json.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class EngineSelectionPlanFixture : public mongo::unittest::Test {
public:
    EngineSelectionPlanFixture()
        : nss(NamespaceString::createNamespaceString_forTest("testdb.coll")) {}

    std::unique_ptr<QuerySolution> makeDistinctScanPlan(BSONObj indexKeys) {
        return makePlan(std::make_unique<DistinctNode>(nss, buildSimpleIndexEntry(indexKeys)));
    }

    std::unique_ptr<QuerySolution> makeIndexScanFetchPlan(BSONObj indexKeys) {
        return makePlan(makeFetchIxScanNode(indexKeys));
    }

    std::unique_ptr<QuerySolution> makePlan(std::unique_ptr<QuerySolutionNode> root) {
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(root));
        return solution;
    }

    std::unique_ptr<QuerySolutionNode> makeFetchIxScanNode(BSONObj indexKeys) {
        auto indexScan = std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(indexKeys));
        return std::make_unique<FetchNode>(std::move(indexScan), nss);
    }

    std::unique_ptr<QuerySolutionNode> makeOrFetchIxScanNode(size_t numBranches) {
        auto orNode = std::make_unique<OrNode>();
        orNode->dedup = false;

        for (size_t i = 0; i < numBranches; ++i) {
            orNode->children.emplace_back(makeFetchIxScanNode(fromjson("{a: 1}")));
        }

        return orNode;
    }

    // Builds FETCH -> SORT_MERGE -> [IXSCAN, ...] (numBranches IXSCAN children).
    // Matches the "Ixscan + SortedMerge + Fetch" pattern in makeLookupUnwindRule.
    std::unique_ptr<QuerySolutionNode> makeFetchSortMergeIxScanNode(size_t numBranches) {
        auto mergeSortNode = std::make_unique<MergeSortNode>();
        mergeSortNode->sort = fromjson("{a: 1}");
        for (size_t i = 0; i < numBranches; ++i) {
            mergeSortNode->children.emplace_back(
                std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}"))));
        }
        return std::make_unique<FetchNode>(std::move(mergeSortNode), nss);
    }

    // Builds SORT_MERGE -> [FETCH -> IXSCAN, ...] (numBranches FETCH+IXSCAN children).
    // Matches the "Ixscan + Fetch + SortedMerge" pattern in makeLookupUnwindRule.
    std::unique_ptr<QuerySolutionNode> makeSortMergeFetchIxScanNode(size_t numBranches) {
        auto mergeSortNode = std::make_unique<MergeSortNode>();
        mergeSortNode->sort = fromjson("{a: 1}");
        for (size_t i = 0; i < numBranches; ++i) {
            mergeSortNode->children.emplace_back(makeFetchIxScanNode(fromjson("{a: 1}")));
        }
        return mergeSortNode;
    }

    std::unique_ptr<QuerySolutionNode> makeFetchSortIxScanNode(BSONObj indexKeys,
                                                               BSONObj sortPattern,
                                                               size_t limit = 0) {
        auto indexScan = std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(indexKeys));
        auto sort = std::make_unique<SortNodeDefault>(
            std::move(indexScan), sortPattern, limit, LimitSkipParameterization::Disabled);
        return std::make_unique<FetchNode>(std::move(sort), nss);
    }

    std::unique_ptr<QuerySolutionNode> makeProjectFetchIxScanNode(
        boost::intrusive_ptr<ExpressionContext> expCtx, BSONObj indexKeys, BSONObj projectionObj) {
        auto projection = projection_ast::parseAndAnalyze(
            expCtx, projectionObj, ProjectionPolicies::findProjectionPolicies());
        return std::make_unique<ProjectionNodeDefault>(
            makeFetchIxScanNode(indexKeys), nullptr, std::move(projection));
    }

    // Builds a $lookup+$unwind (LU) node with the given outer child and join strategy. For
    // strategies that require specific inner-side children (INLJ, DINLJ), appropriate children are
    // created automatically.
    std::unique_ptr<QuerySolutionNode> makeLuNodeWithStrategy(
        std::unique_ptr<QuerySolutionNode> outerChild, EqLookupNode::LookupStrategy strategy) {
        auto nssForeign = NamespaceString::createNamespaceString_forTest("testdb.collForeign");

        std::vector<std::unique_ptr<QuerySolutionNode>> children;
        children.emplace_back(std::move(outerChild));

        // INLJ and DINLJ require the inner child to be a FETCH+IXSCAN (the index side of the join).
        // kNonExistentForeignCollection uses an EofNode (the foreign collection doesn't exist).
        if (strategy == EqLookupNode::LookupStrategy::kIndexedLoopJoin ||
            strategy == EqLookupNode::LookupStrategy::kDynamicIndexedLoopJoin) {
            children.emplace_back(std::make_unique<FetchNode>(
                std::make_unique<IndexScanNode>(nssForeign,
                                                buildSimpleIndexEntry(fromjson("{b: 1}"))),
                nssForeign));
        } else if (strategy == EqLookupNode::LookupStrategy::kNonExistentForeignCollection) {
            children.emplace_back(
                std::make_unique<EofNode>(eof_node::EOFType::NonExistentNamespace));
        } else {
            children.emplace_back(std::make_unique<CollectionScanNode>(nssForeign));
        }

        // DINLJ additionally needs a third child (the collscan fallback stream).
        if (strategy == EqLookupNode::LookupStrategy::kDynamicIndexedLoopJoin) {
            children.emplace_back(std::make_unique<CollectionScanNode>(nssForeign));
        }

        return std::make_unique<EqLookupNode>(std::move(children),
                                              nssForeign,
                                              FieldPath("b"),
                                              FieldPath("c"),
                                              FieldPath("a"),
                                              strategy,
                                              false,
                                              false,
                                              boost::none);
    }

    // Builds a minimal GROUP node (count) wrapping 'child'.
    std::unique_ptr<GroupNode> makeSimpleGroupNode(std::unique_ptr<QuerySolutionNode> child,
                                                   boost::intrusive_ptr<ExpressionContext> expCtx) {
        auto countSpec = fromjson("{n: {$count: {}}}");
        return std::make_unique<GroupNode>(
            std::move(child),
            ExpressionConstant::create(expCtx.get(), Value(BSONNULL)),
            std::vector<AccumulationStatement>{AccumulationStatement::parseAccumulationStatement(
                expCtx.get(), countSpec["n"], expCtx->variablesParseState)},
            false /* merging */,
            false /* willBeMerged */,
            true /* shouldProduceBson */);
    }

protected:
    NamespaceString nss;
};

// Test selection of IXSCAN + FETCH + LU plans.
TEST_F(EngineSelectionPlanFixture, LookupUnwindFetchIxScanSelection) {
    auto dataAccess = makeFetchIxScanNode(fromjson("{a: 1}"));
    const auto* dataAccessNode = dataAccess.get();
    auto solution = makePlan(
        makeLuNodeWithStrategy(std::move(dataAccess), EqLookupNode::LookupStrategy::kHashJoin));

    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode, ctx);
    ASSERT_EQ(result.engine, EngineChoice::kSbe);
    ASSERT_EQ(result.planPushdownRoot, solution->root());
}

// Test eligibility of DISTINCT_SCAN plans.
TEST_F(EngineSelectionPlanFixture, DistinctScanEligibility) {
    BSONObj indexFields = fromjson("{a: 1}");

    std::unique_ptr<QuerySolution> solution = makeDistinctScanPlan(indexFields);
    ASSERT_FALSE(isPlanSbeCompatible(solution.get()));
}

// Test eligibility of FETCH + IXSCAN plans with hashed indexes.
TEST_F(EngineSelectionPlanFixture, HashedIndexIxScanEligibility) {
    // Hashed index containing the SERVER-99889 pattern.
    {
        BSONObj indexFields = fromjson("{a: 1, m: 'hashed', 'm.m1': 1}");
        std::unique_ptr<QuerySolution> solution = makeIndexScanFetchPlan(indexFields);
        ASSERT_FALSE(isPlanSbeCompatible(solution.get()));
    }

    // Single hashed index.
    {
        BSONObj indexFields = fromjson("{a: 'hashed'}");
        std::unique_ptr<QuerySolution> solution = makeIndexScanFetchPlan(indexFields);
        ASSERT_TRUE(isPlanSbeCompatible(solution.get()));
    }
}

// Test eligibility of AND_HASH plans.
TEST_F(EngineSelectionPlanFixture, AndHashEligibility) {
    auto andHash = std::make_unique<AndHashNode>();
    andHash->children.emplace_back(
        std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}"))));
    andHash->children.emplace_back(
        std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{b: 1}"))));

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(andHash));
    ASSERT_FALSE(isPlanSbeCompatible(solution.get()));
}

// Test eligibility of AND_SORTED plans.
TEST_F(EngineSelectionPlanFixture, AndSortedEligibility) {
    auto andSorted = std::make_unique<AndSortedNode>();
    andSorted->children.emplace_back(
        std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}"))));
    andSorted->children.emplace_back(
        std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{b: 1}"))));

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(andSorted));
    ASSERT_FALSE(isPlanSbeCompatible(solution.get()));
}

// Test selection of IXSCAN + FETCH plans.
TEST_F(EngineSelectionPlanFixture, FetchIxScanSelection) {
    auto dataAccess = makeFetchIxScanNode(fromjson("{a: 1}"));
    const auto* dataAccessNode = dataAccess.get();
    std::unique_ptr<QuerySolution> solution = makePlan(std::move(dataAccess));

    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode, ctx);
    ASSERT_EQ(result.engine, EngineChoice::kClassic);
    ASSERT_EQ(result.planPushdownRoot, nullptr);
}

// Test selection of IXSCAN + FETCH + OR + LU plans.
TEST_F(EngineSelectionPlanFixture, LookupUnwindOrFetchIxScanSelection) {
    auto runTest = [this](int numBranches, EngineChoice engine) {
        auto dataAccess = makeOrFetchIxScanNode(numBranches);
        const auto* dataAccessNode = dataAccess.get();
        auto solution = makePlan(
            makeLuNodeWithStrategy(std::move(dataAccess), EqLookupNode::LookupStrategy::kHashJoin));

        IncrementalFeatureRolloutContext ctx;
        EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode, ctx);
        ASSERT_EQ(result.engine, engine);
        ASSERT_EQ(result.planPushdownRoot,
                  engine == EngineChoice::kClassic ? nullptr : solution->root());
    };
    runTest(1, EngineChoice::kSbe);
    runTest(3, EngineChoice::kSbe);
    runTest(101, EngineChoice::kClassic);
}

// Test selection of IXSCAN + SORT + FETCH + LU + PROJECT + MATCH plans.
TEST_F(EngineSelectionPlanFixture, MatchProjectLookupUnwindFetchSortIxScanSelection) {
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(nss).build();

    auto dataAccess = makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"));
    const auto* dataAccessNode = dataAccess.get();
    auto lookupUnwindNode =
        makeLuNodeWithStrategy(std::move(dataAccess), EqLookupNode::LookupStrategy::kHashJoin);

    auto projection = projection_ast::parseAndAnalyze(
        expCtx, fromjson("{computedA: '$a'}"), ProjectionPolicies::addFieldsProjectionPolicies());
    auto project = std::make_unique<ProjectionNodeDefault>(
        std::move(lookupUnwindNode), nullptr, std::move(projection));

    auto matchNode = std::make_unique<MatchNode>(std::move(project),
                                                 unittest::assertGet(MatchExpressionParser::parse(
                                                     fromjson("{computedA: {$gte: 0}}"), expCtx)));

    auto solution = makePlan(std::move(matchNode));

    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode, ctx);
    ASSERT_EQ(result.engine, EngineChoice::kSbe);
    ASSERT_EQ(result.planPushdownRoot, solution->root());
}

TEST_F(EngineSelectionPlanFixture, LookupUnwindFetchSortIxScanWithAbsorbedLimitSelection) {
    auto dataAccess =
        makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"), 1 /* limit */);
    const auto* dataAccessNode = dataAccess.get();
    auto solution = makePlan(
        makeLuNodeWithStrategy(std::move(dataAccess), EqLookupNode::LookupStrategy::kHashJoin));

    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode, ctx);
    ASSERT_EQ(result.engine, EngineChoice::kClassic);
    ASSERT_EQ(result.planPushdownRoot, nullptr);
}

// Test selection of IXSCAN + FETCH + PROJECT + GROUP + LU plans.
TEST_F(EngineSelectionPlanFixture, GroupLookupUnwindProjectFetchIxScanSelection) {
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(nss).build();

    auto dataAccess =
        makeProjectFetchIxScanNode(expCtx, fromjson("{a: 1}"), fromjson("{a: 1, _id: 0}"));
    const auto* dataAccessNode = dataAccess.get();

    auto countSpec = fromjson("{count: {$count: {}}}");
    VariablesParseState vps = expCtx->variablesParseState;
    auto groupNode = std::make_unique<GroupNode>(
        std::move(dataAccess),
        ExpressionConstant::create(expCtx.get(), Value(BSONNULL)),
        std::vector<AccumulationStatement>{AccumulationStatement::parseAccumulationStatement(
            expCtx.get(), countSpec["count"], vps)},
        false,
        false,
        true);
    const auto* groupNodePtr = groupNode.get();

    auto lookupUnwindNode =
        makeLuNodeWithStrategy(std::move(groupNode), EqLookupNode::LookupStrategy::kHashJoin);

    auto solution = makePlan(std::move(lookupUnwindNode));

    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode, ctx);
    ASSERT_EQ(result.engine, EngineChoice::kSbe);
    ASSERT_EQ(result.planPushdownRoot, groupNodePtr);
}

// When all flags are on (the default), every combination of join strategy and local access plan
// runs in SBE.
TEST_F(EngineSelectionPlanFixture, LuAllFlagsEnabledSelectsSbeForAllCombinations) {
    using Strategy = EqLookupNode::LookupStrategy;
    const Strategy strategies[] = {
        Strategy::kHashJoin,
        Strategy::kIndexedLoopJoin,
        Strategy::kNestedLoopJoin,
        Strategy::kDynamicIndexedLoopJoin,
    };

    auto runCombination = [&](StringData label,
                              std::unique_ptr<QuerySolutionNode> dataAccessNode,
                              Strategy strategy) {
        const auto* dan = dataAccessNode.get();
        auto solution = makePlan(makeLuNodeWithStrategy(std::move(dataAccessNode), strategy));
        IncrementalFeatureRolloutContext ctx;
        EngineSelectionResult result = engineSelectionForPlan(solution.get(), dan, ctx);
        auto strategyStr = EqLookupNode::serializeLookupStrategy(strategy);
        ASSERT_EQ(result.engine, EngineChoice::kSbe)
            << label << " + " << strategyStr << ": expected SBE";
        ASSERT_EQ(result.planPushdownRoot, solution->root())
            << label << " + " << strategyStr << ": wrong pushdown root";
    };

    for (auto strategy : strategies) {
        runCombination("COLLSCAN", std::make_unique<CollectionScanNode>(nss), strategy);
        runCombination("IXSCAN+FETCH", makeFetchIxScanNode(fromjson("{a: 1}")), strategy);
        runCombination("SORT+IXSCAN+FETCH",
                       makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}")),
                       strategy);
    }
}

// IFR flags: isolation - local-side flags must not affect non-LU queries.

// Disabling the IXSCAN+FETCH LU flag must NOT affect a plain IXSCAN+FETCH query without $LU.
TEST_F(EngineSelectionPlanFixture, LuIxscanFetchFlagDoesNotAffectNonLuQuery) {
    unittest::ServerParameterGuard flagOff{"featureFlagSbeEqLookupUnwindLocalIxscanFetch", false};

    // A plain IXSCAN+FETCH without any LU node is already Classic (no GROUP/EqLookup to push
    // down), and disabling the LU access plan flag must not change that result.
    auto solution = makePlan(makeFetchIxScanNode(fromjson("{a: 1}")));
    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), solution->root(), ctx);
    ASSERT_EQ(result.engine, EngineChoice::kClassic);
    ASSERT_EQ(result.planPushdownRoot, nullptr);

    // GROUP wrapping the same IXSCAN+FETCH still runs SBE; the flag only gates LU.
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(nss).build();
    auto groupNode = makeSimpleGroupNode(makeFetchIxScanNode(fromjson("{a: 1}")), expCtx);
    const auto* groupPtr = groupNode.get();
    auto sol2 = makePlan(std::move(groupNode));
    IncrementalFeatureRolloutContext ctx2;
    auto result2 = engineSelectionForPlan(sol2.get(), sol2->root(), ctx2);
    ASSERT_EQ(result2.engine, EngineChoice::kSbe);
    ASSERT_EQ(result2.planPushdownRoot, groupPtr);
}

// Disabling the COLLSCAN LU flag must NOT affect a plain COLLSCAN query without $LU.
TEST_F(EngineSelectionPlanFixture, LuCollscanFlagDoesNotAffectNonLuQuery) {
    unittest::ServerParameterGuard flagOff{"featureFlagSbeEqLookupUnwindLocalCollscan", false};

    // A plain COLLSCAN without any LU node stays Classic regardless of the LU collscan flag.
    auto solution = makePlan(std::make_unique<CollectionScanNode>(nss));
    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), solution->root(), ctx);
    ASSERT_EQ(result.engine, EngineChoice::kClassic);
    ASSERT_EQ(result.planPushdownRoot, nullptr);

    // GROUP wrapping the same COLLSCAN still runs SBE; the flag only gates LU.
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(nss).build();
    auto groupNode = makeSimpleGroupNode(std::make_unique<CollectionScanNode>(nss), expCtx);
    const auto* groupPtr = groupNode.get();
    auto sol2 = makePlan(std::move(groupNode));
    IncrementalFeatureRolloutContext ctx2;
    auto result2 = engineSelectionForPlan(sol2.get(), sol2->root(), ctx2);
    ASSERT_EQ(result2.engine, EngineChoice::kSbe);
    ASSERT_EQ(result2.planPushdownRoot, groupPtr);
}

// Disabling featureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans must NOT affect a plain
// complex data-access query without $LU.
TEST_F(EngineSelectionPlanFixture, LuComplexFlagDoesNotAffectNonLuQuery) {
    unittest::ServerParameterGuard flagOff{
        "featureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans", false};

    // A plain FETCH+SORT+IXSCAN without any LU node stays Classic regardless of the complex flag.
    auto solution = makePlan(makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}")));
    IncrementalFeatureRolloutContext ctx;
    EngineSelectionResult result = engineSelectionForPlan(solution.get(), solution->root(), ctx);
    ASSERT_EQ(result.engine, EngineChoice::kClassic);
    ASSERT_EQ(result.planPushdownRoot, nullptr);

    // GROUP wrapping the same data access still runs SBE; the flag only gates LU.
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(nss).build();
    auto groupNode = makeSimpleGroupNode(
        makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}")), expCtx);
    const auto* groupPtr = groupNode.get();
    auto sol2 = makePlan(std::move(groupNode));
    IncrementalFeatureRolloutContext ctx2;
    auto result2 = engineSelectionForPlan(sol2.get(), sol2->root(), ctx2);
    ASSERT_EQ(result2.engine, EngineChoice::kSbe);
    ASSERT_EQ(result2.planPushdownRoot, groupPtr);
}

// Verifies all 7 listed access-plan patterns are recognised and a representative set of
// unlisted patterns are correctly rejected.
TEST_F(EngineSelectionPlanFixture, LuRuleMatchesListedPatternsAndRejectsOthers) {
    // Runs the data access node through engineSelectionForPlan with an LU wrapper and returns
    // the engine choice, allowing concise match/reject assertions below.
    auto runRule = [&](std::unique_ptr<QuerySolutionNode> dataAccess) {
        const auto* dan = dataAccess.get();
        auto solution = makePlan(
            makeLuNodeWithStrategy(std::move(dataAccess), EqLookupNode::LookupStrategy::kHashJoin));
        IncrementalFeatureRolloutContext ctx;
        return engineSelectionForPlan(solution.get(), dan, ctx).engine;
    };

    // Listed patterns: all should match (SBE).

    // 1. COLLSCAN
    ASSERT_EQ(runRule(std::make_unique<CollectionScanNode>(nss)), EngineChoice::kSbe);

    // 2. FETCH -> IXSCAN
    ASSERT_EQ(runRule(makeFetchIxScanNode(fromjson("{a: 1}"))), EngineChoice::kSbe);

    // 3. FETCH -> SORT_DEFAULT -> IXSCAN (no absorbed limit)
    ASSERT_EQ(runRule(makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"))),
              EngineChoice::kSbe);

    // 4. FETCH -> OR -> IXSCAN (2 branches)
    {
        auto orNode = std::make_unique<OrNode>();
        orNode->dedup = false;
        orNode->children.emplace_back(
            std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}"))));
        orNode->children.emplace_back(
            std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}"))));
        ASSERT_EQ(runRule(std::make_unique<FetchNode>(std::move(orNode), nss)), EngineChoice::kSbe);
    }

    // 5. OR -> FETCH -> IXSCAN (1 and 3 branches)
    ASSERT_EQ(runRule(makeOrFetchIxScanNode(1)), EngineChoice::kSbe);
    ASSERT_EQ(runRule(makeOrFetchIxScanNode(3)), EngineChoice::kSbe);

    // 6. FETCH -> SORT_MERGE -> IXSCAN
    ASSERT_EQ(runRule(makeFetchSortMergeIxScanNode(2)), EngineChoice::kSbe);

    // 7. SORT_MERGE -> FETCH -> IXSCAN
    ASSERT_EQ(runRule(makeSortMergeFetchIxScanNode(2)), EngineChoice::kSbe);

    // Unlisted patterns: all should be rejected (Classic).

    // FETCH -> SORT_DEFAULT -> IXSCAN with absorbed limit
    ASSERT_EQ(runRule(makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"), 1 /*limit*/)),
              EngineChoice::kClassic);

    // OR -> FETCH -> IXSCAN with more than 100 branches
    ASSERT_EQ(runRule(makeOrFetchIxScanNode(101)), EngineChoice::kClassic);

    // Plain IXSCAN without a FETCH wrapper
    ASSERT_EQ(
        runRule(std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}")))),
        EngineChoice::kClassic);

    // FETCH -> SORT_DEFAULT -> IXSCAN with absorbed limit (sort-merge variant)
    {
        auto innerIxscan =
            std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}")));
        auto sort = std::make_unique<SortNodeDefault>(
            std::move(innerIxscan), fromjson("{a: 1}"), 5, LimitSkipParameterization::Disabled);
        ASSERT_EQ(runRule(std::make_unique<FetchNode>(std::move(sort), nss)),
                  EngineChoice::kClassic);
    }

    // OR with mixed covered/uncovered branches: doesn't match pattern 4 (FETCH->OR->IXSCAN) or
    // pattern 5 (OR->FETCH->IXSCAN) since the branch shapes are inconsistent.
    {
        auto orNode = std::make_unique<OrNode>();
        orNode->dedup = false;
        orNode->children.emplace_back(makeFetchIxScanNode(fromjson("{a: 1}")));
        orNode->children.emplace_back(
            std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(fromjson("{a: 1}"))));
        ASSERT_EQ(runRule(std::move(orNode)), EngineChoice::kClassic);
    }
}

// For each of the 7 IFR flags, disabling it must fall back exactly the combinations that are gated
// by that flag, no more, no less.
TEST_F(EngineSelectionPlanFixture, LuEachFlagDisablesExactlyExpectedCombinations) {
    using Strategy = EqLookupNode::LookupStrategy;

    // Each combo carries pointers to the flags that gate it; disabling either causes Classic.
    struct Combo {
        std::string label;
        Strategy strategy;
        IncrementalRolloutFeatureFlag* strategyFlag;
        IncrementalRolloutFeatureFlag* accessPlanFlag;
        std::function<std::unique_ptr<QuerySolutionNode>()> makeDataAccess;
    };

    const Combo matrix[] = {
        // COLLSCAN access plan
        {"HJ+COLLSCAN",
         Strategy::kHashJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindHashJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalCollscan,
         [&] {
             return std::make_unique<CollectionScanNode>(nss);
         }},
        {"INLJ+COLLSCAN",
         Strategy::kIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalCollscan,
         [&] {
             return std::make_unique<CollectionScanNode>(nss);
         }},
        {"NLJ+COLLSCAN",
         Strategy::kNestedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindNestedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalCollscan,
         [&] {
             return std::make_unique<CollectionScanNode>(nss);
         }},
        {"DINLJ+COLLSCAN",
         Strategy::kDynamicIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalCollscan,
         [&] {
             return std::make_unique<CollectionScanNode>(nss);
         }},
        // IXSCAN+FETCH access plan
        {"HJ+IXSCAN+FETCH",
         Strategy::kHashJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindHashJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalIxscanFetch,
         [&] {
             return makeFetchIxScanNode(fromjson("{a: 1}"));
         }},
        {"INLJ+IXSCAN+FETCH",
         Strategy::kIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalIxscanFetch,
         [&] {
             return makeFetchIxScanNode(fromjson("{a: 1}"));
         }},
        {"NLJ+IXSCAN+FETCH",
         Strategy::kNestedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindNestedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalIxscanFetch,
         [&] {
             return makeFetchIxScanNode(fromjson("{a: 1}"));
         }},
        {"DINLJ+IXSCAN+FETCH",
         Strategy::kDynamicIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalIxscanFetch,
         [&] {
             return makeFetchIxScanNode(fromjson("{a: 1}"));
         }},
        // SORT+IXSCAN+FETCH (complex) access plan
        {"HJ+SORT+IXSCAN+FETCH",
         Strategy::kHashJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindHashJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans,
         [&] {
             return makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"));
         }},
        {"INLJ+SORT+IXSCAN+FETCH",
         Strategy::kIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans,
         [&] {
             return makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"));
         }},
        {"NLJ+SORT+IXSCAN+FETCH",
         Strategy::kNestedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindNestedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans,
         [&] {
             return makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"));
         }},
        {"DINLJ+SORT+IXSCAN+FETCH",
         Strategy::kDynamicIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin,
         &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans,
         [&] {
             return makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}"));
         }},
    };

    IncrementalRolloutFeatureFlag* allFlags[] = {
        &feature_flags::gFeatureFlagSbeEqLookupUnwindHashJoin,
        &feature_flags::gFeatureFlagSbeEqLookupUnwindIndexedLoopJoin,
        &feature_flags::gFeatureFlagSbeEqLookupUnwindNestedLoopJoin,
        &feature_flags::gFeatureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin,
        &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalCollscan,
        &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalIxscanFetch,
        &feature_flags::gFeatureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans,
    };

    for (auto* flag : allFlags) {
        unittest::ServerParameterGuard flagOff{flag->getName(), false};
        for (const auto& combo : matrix) {
            const bool expectClassic = (combo.strategyFlag == flag || combo.accessPlanFlag == flag);
            auto dataAccess = combo.makeDataAccess();
            const auto* dan = dataAccess.get();
            auto solution = makePlan(makeLuNodeWithStrategy(std::move(dataAccess), combo.strategy));
            IncrementalFeatureRolloutContext ctx;
            auto result = engineSelectionForPlan(solution.get(), dan, ctx);
            ASSERT_EQ(result.engine, expectClassic ? EngineChoice::kClassic : EngineChoice::kSbe)
                << "flag=" << flag->getName() << " combo=" << combo.label;
        }
    }
}

TEST_F(EngineSelectionPlanFixture, LuNonExistentForeignCollectionFlagGatedByNlj) {
    using Strategy = EqLookupNode::LookupStrategy;

    // With NLJ flag on (default): NonExistentForeignCollection runs in SBE.
    {
        auto dataAccess = std::make_unique<CollectionScanNode>(nss);
        const auto* dan = dataAccess.get();
        auto solution = makePlan(
            makeLuNodeWithStrategy(std::move(dataAccess), Strategy::kNonExistentForeignCollection));
        IncrementalFeatureRolloutContext ctx;
        EngineSelectionResult result = engineSelectionForPlan(solution.get(), dan, ctx);
        ASSERT_EQ(result.engine, EngineChoice::kSbe);
    }

    // With NLJ flag off: NonExistentForeignCollection falls back to Classic.
    {
        unittest::ServerParameterGuard flagOff{"featureFlagSbeEqLookupUnwindNestedLoopJoin", false};
        auto dataAccess = std::make_unique<CollectionScanNode>(nss);
        const auto* dan = dataAccess.get();
        auto solution = makePlan(
            makeLuNodeWithStrategy(std::move(dataAccess), Strategy::kNonExistentForeignCollection));
        IncrementalFeatureRolloutContext ctx;
        EngineSelectionResult result = engineSelectionForPlan(solution.get(), dan, ctx);
        ASSERT_EQ(result.engine, EngineChoice::kClassic);
        ASSERT_EQ(result.planPushdownRoot, nullptr);
    }
}

}  // namespace mongo
