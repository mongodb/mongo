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
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/db/query/query_test_service_context.h"
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

    std::unique_ptr<QuerySolutionNode> makeSentinelNode(std::unique_ptr<QuerySolutionNode> child) {
        auto sentinel = std::make_unique<SentinelNode>();
        sentinel->children.emplace_back(std::move(child));
        return sentinel;
    }

    std::unique_ptr<QuerySolutionNode> makeFetchSortIxScanNode(BSONObj indexKeys,
                                                               BSONObj sortPattern) {
        auto indexScan = std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(indexKeys));
        auto sort = std::make_unique<SortNodeDefault>(
            std::move(indexScan), sortPattern, 0, LimitSkipParameterization::Disabled);
        return std::make_unique<FetchNode>(std::move(sort), nss);
    }

    std::unique_ptr<QuerySolutionNode> makeProjectFetchIxScanNode(
        boost::intrusive_ptr<ExpressionContext> expCtx, BSONObj indexKeys, BSONObj projectionObj) {
        auto projection = projection_ast::parseAndAnalyze(
            expCtx, projectionObj, ProjectionPolicies::findProjectionPolicies());
        return std::make_unique<ProjectionNodeDefault>(
            makeFetchIxScanNode(indexKeys), nullptr, std::move(projection));
    }

    std::unique_ptr<QuerySolutionNode> makeLookupUnwindNode(
        std::unique_ptr<QuerySolutionNode> child) {
        auto nssForeign = NamespaceString::createNamespaceString_forTest("testdb.collForeign");

        std::vector<std::unique_ptr<QuerySolutionNode>> children;
        children.emplace_back(std::move(child));
        children.emplace_back(std::make_unique<CollectionScanNode>(nssForeign));

        return std::make_unique<EqLookupNode>(std::move(children),
                                              nssForeign,
                                              FieldPath("b"),
                                              FieldPath("c"),
                                              FieldPath("a"),
                                              EqLookupNode::LookupStrategy::kHashJoin,
                                              false,
                                              false,
                                              boost::none);
    }

protected:
    NamespaceString nss;
};

// Test selection of IXSCAN + FETCH + LU plans.
TEST_F(EngineSelectionPlanFixture, LookupUnwindFetchIxScanSelection) {
    BSONObj indexFields = fromjson("{a: 1}");
    auto sentinel = makeSentinelNode(makeFetchIxScanNode(indexFields));
    const auto* dataAccessNode = sentinel->children[0].get();
    auto lookupUnwind = makeLookupUnwindNode(std::move(sentinel));
    auto solution = makePlan(std::move(lookupUnwind));

    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode);
    ASSERT_EQ(result.engine, EngineChoice::kSbe);
    ASSERT_EQ(result.planPushdownRoot, solution->root());
}

// Test eligibility of DISTINCT_SCAN plans.
TEST_F(EngineSelectionPlanFixture, DistinctScanEligibility) {
    BSONObj indexFields = fromjson("{a: 1}");

    std::unique_ptr<QuerySolution> solution = makeDistinctScanPlan(indexFields);
    ASSERT_FALSE(isPlanSbeEligible(solution.get()));
}

// Test eligibility of FETCH + IXSCAN plans with hashed indexes.
TEST_F(EngineSelectionPlanFixture, HashedIndexIxScanEligibility) {
    // Hashed index containing the SERVER-99889 pattern.
    {
        BSONObj indexFields = fromjson("{a: 1, m: 'hashed', 'm.m1': 1}");
        std::unique_ptr<QuerySolution> solution = makeIndexScanFetchPlan(indexFields);
        ASSERT_FALSE(isPlanSbeEligible(solution.get()));
    }

    // Single hashed index.
    {
        BSONObj indexFields = fromjson("{a: 'hashed'}");
        std::unique_ptr<QuerySolution> solution = makeIndexScanFetchPlan(indexFields);
        ASSERT_TRUE(isPlanSbeEligible(solution.get()));
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
    ASSERT_FALSE(isPlanSbeEligible(solution.get()));
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
    ASSERT_FALSE(isPlanSbeEligible(solution.get()));
}

// Test selection of IXSCAN + FETCH plans.
TEST_F(EngineSelectionPlanFixture, FetchIxScanSelection) {
    BSONObj indexFields = fromjson("{a: 1}");

    auto sentinel = makeSentinelNode(makeFetchIxScanNode(indexFields));
    const auto* dataAccessNode = sentinel->children[0].get();
    std::unique_ptr<QuerySolution> solution = makePlan(std::move(sentinel));

    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode);
    ASSERT_EQ(result.engine, EngineChoice::kClassic);
    ASSERT_EQ(result.planPushdownRoot, nullptr);
}

// Test selection of IXSCAN + FETCH + OR + LU plans.
TEST_F(EngineSelectionPlanFixture, LookupUnwindOrFetchIxScanSelection) {
    auto runTest = [this](int numBranches, EngineChoice engine) {
        auto sentinel = makeSentinelNode(makeOrFetchIxScanNode(numBranches));
        const auto* dataAccessNode = sentinel->children[0].get();
        auto solution = makePlan(makeLookupUnwindNode(std::move(sentinel)));

        EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode);
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

    auto sentinel =
        makeSentinelNode(makeFetchSortIxScanNode(fromjson("{a: 1}"), fromjson("{a: 1}")));
    const auto* dataAccessNode = sentinel->children[0].get();
    auto lookupUnwindNode = makeLookupUnwindNode(std::move(sentinel));

    auto projection = projection_ast::parseAndAnalyze(
        expCtx, fromjson("{computedA: '$a'}"), ProjectionPolicies::addFieldsProjectionPolicies());
    auto project = std::make_unique<ProjectionNodeDefault>(
        std::move(lookupUnwindNode), nullptr, std::move(projection));

    auto matchNode = std::make_unique<MatchNode>(std::move(project),
                                                 unittest::assertGet(MatchExpressionParser::parse(
                                                     fromjson("{computedA: {$gte: 0}}"), expCtx)));

    auto solution = makePlan(std::move(matchNode));

    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode);
    ASSERT_EQ(result.engine, EngineChoice::kSbe);
    ASSERT_EQ(result.planPushdownRoot, solution->root());
}

// Test selection of IXSCAN + FETCH + PROJECT + GROUP + LU plans.
TEST_F(EngineSelectionPlanFixture, GroupLookupUnwindProjectFetchIxScanSelection) {
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(nss).build();

    auto sentinel = makeSentinelNode(
        makeProjectFetchIxScanNode(expCtx, fromjson("{a: 1}"), fromjson("{a: 1, _id: 0}")));
    const auto* dataAccessNode = sentinel->children[0].get();

    auto countSpec = fromjson("{count: {$count: {}}}");
    VariablesParseState vps = expCtx->variablesParseState;
    auto groupNode = std::make_unique<GroupNode>(
        std::move(sentinel),
        ExpressionConstant::create(expCtx.get(), Value(BSONNULL)),
        std::vector<AccumulationStatement>{AccumulationStatement::parseAccumulationStatement(
            expCtx.get(), countSpec["count"], vps)},
        false,
        false,
        true);
    const auto* groupNodePtr = groupNode.get();

    auto lookupUnwindNode = makeLookupUnwindNode(std::move(groupNode));

    auto solution = makePlan(std::move(lookupUnwindNode));

    EngineSelectionResult result = engineSelectionForPlan(solution.get(), dataAccessNode);
    ASSERT_EQ(result.engine, EngineChoice::kSbe);
    ASSERT_EQ(result.planPushdownRoot, groupNodePtr);
}

}  // namespace mongo
