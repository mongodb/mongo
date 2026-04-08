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
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class EngineSelectionPlanFixture : public mongo::unittest::Test {
public:
    EngineSelectionPlanFixture()
        : nss(NamespaceString::createNamespaceString_forTest("testdb.coll")) {}

    std::unique_ptr<QuerySolution> makeDistinctScanPlan(BSONObj indexKeys) {
        auto distinct = std::make_unique<DistinctNode>(nss, buildSimpleIndexEntry(indexKeys));
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(distinct));
        return solution;
    }

    std::unique_ptr<QuerySolution> makeIndexScanFetchPlan(BSONObj indexKeys) {
        auto indexScan = std::make_unique<IndexScanNode>(nss, buildSimpleIndexEntry(indexKeys));
        auto fetch = std::make_unique<FetchNode>(std::move(indexScan), nss);

        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(fetch));
        return solution;
    }

protected:
    NamespaceString nss;
};

TEST_F(EngineSelectionPlanFixture, LookupUnwind) {
    auto nssLocal = NamespaceString::createNamespaceString_forTest("testdb.collLocal");
    auto nssForeign = NamespaceString::createNamespaceString_forTest("testdb.collForeign");

    BSONObj indexFields = fromjson("{a: 1}");
    std::vector<std::unique_ptr<QuerySolutionNode>> children;
    children.emplace_back(
        std::make_unique<IndexScanNode>(nssLocal, buildSimpleIndexEntry(indexFields)));
    children.emplace_back(std::make_unique<CollectionScanNode>(nssForeign));
    auto lookupUnwind = std::make_unique<EqLookupNode>(std::move(children),
                                                       nssForeign,
                                                       FieldPath("b"),
                                                       FieldPath("c"),
                                                       FieldPath("a"),
                                                       EqLookupNode::LookupStrategy::kHashJoin,
                                                       false,
                                                       false,
                                                       boost::none);
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(lookupUnwind));

    EngineSelectionResult result = engineSelectionForPlan(solution.get());
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

// Test selection of FETCH + IXSCAN plans.
TEST_F(EngineSelectionPlanFixture, FetchIxScanSelection) {
    BSONObj indexFields = fromjson("{a: 1}");

    std::unique_ptr<QuerySolution> solution = makeIndexScanFetchPlan(indexFields);

    EngineSelectionResult result = engineSelectionForPlan(solution.get());
    ASSERT_EQ(result.engine, EngineChoice::kClassic);
    ASSERT_EQ(result.planPushdownRoot, nullptr);
}

}  // namespace mongo
