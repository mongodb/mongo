/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/shard_filterer_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/db/query/shard_filterer_factory_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

const NamespaceString kTestNss("TestDB", "TestColl");

class SbeShardFilterTest : public SbeStageBuilderTestFixture {
protected:
    /**
     * Makes a ShardFiltererFactoryInterface that produces a mock ShardFilterer that always passes.
     */
    std::unique_ptr<ShardFiltererFactoryInterface> makeAlwaysPassShardFiltererFactory(
        const BSONObj& shardKeyPattern) {
        return std::make_unique<ShardFiltererFactoryMock>(
            std::make_unique<ConstantFilterMock>(true, shardKeyPattern));
    }

    /**
     * Makes a ShardFiltererFactoryInterface that produces a mock ShardFilterer that always fails.
     */
    std::unique_ptr<ShardFiltererFactoryInterface> makeAlwaysFailShardFiltererFactory(
        const BSONObj& shardKeyPattern) {
        return std::make_unique<ShardFiltererFactoryMock>(
            std::make_unique<ConstantFilterMock>(false, shardKeyPattern));
    }

    /**
     * Makes a ShardFiltererFactoryInterface that produces a mock ShardFilterer to filter out docs
     * containing all null values along the shard key.
     */
    std::unique_ptr<ShardFiltererFactoryInterface> makeAllNullShardKeyFiltererFactory(
        const BSONObj& shardKeyPattern) {
        return std::make_unique<ShardFiltererFactoryMock>(
            std::make_unique<AllNullShardKeyFilterMock>(shardKeyPattern));
    }


    /**
     * Makes a new QuerySolutionNode consisting of a ShardingFilterNode and a child VirtualScanNode.
     */
    std::unique_ptr<QuerySolutionNode> makeFilterVirtualScanTree(std::vector<BSONArray> docs) {
        auto virtScan =
            std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);
        auto shardFilter = std::make_unique<ShardingFilterNode>();
        shardFilter->children.push_back(virtScan.release());
        return std::move(shardFilter);
    }

    /**
     * Runs a unit test with a given shard filterer factory and asserts that the results match the
     * expected docs.
     */
    void runTest(std::vector<BSONArray> docs,
                 const BSONArray& expected,
                 std::unique_ptr<ShardFiltererFactoryInterface> shardFiltererFactory) {
        // Construct a QuerySolutionNode consisting of a ShardingFilterNode with a single child
        // VirtualScanNode.
        auto shardFilter = makeFilterVirtualScanTree(docs);
        runTest(std::move(shardFilter), expected, std::move(shardFiltererFactory));
    }

    /**
     * Similar to the above, but rather than hardcoding a SHARDING_FILTER => VIRTUAL_SCAN query
     * solution, uses the query solution node tree provided by the caller.
     */
    void runTest(std::unique_ptr<QuerySolutionNode> qsn,
                 const BSONArray& expected,
                 std::unique_ptr<ShardFiltererFactoryInterface> shardFiltererFactory) {
        auto querySolution = makeQuerySolution(std::move(qsn));

        // Translate the QuerySolution to an sbe::PlanStage.
        auto [resultSlots, stage, data] =
            buildPlanStage(std::move(querySolution), false, std::move(shardFiltererFactory));

        // Prepare the sbe::PlanStage for execution and collect all results.
        auto resultAccessors = prepareTree(&data.ctx, stage.get(), resultSlots);
        auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessors[0]);
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        // Convert the expected results to an sbe value and assert results.
        auto [expectedTag, expectedVal] = stage_builder::makeValue(expected);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
    }
};

TEST_F(SbeShardFilterTest, AlwaysPassFilter) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 2)
                               << BSON("a" << 2 << "b" << 2) << BSON("a" << 3 << "b" << 2));
    runTest(docs, expected, makeAlwaysPassShardFiltererFactory(BSON("a" << 1)));
}

TEST_F(SbeShardFilterTest, AlwaysFailFilter) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto expected = BSONArray();
    runTest(docs, expected, makeAlwaysFailShardFiltererFactory(BSON("a" << 1)));
}

TEST_F(SbeShardFilterTest, ArrayAlongLeafShardKeyGetsFiltered) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << BSON_ARRAY(1 << 2)))};

    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 2) << BSON("a" << 2 << "b" << 2));
    runTest(docs, expected, makeAlwaysPassShardFiltererFactory(BSON("a" << 1 << "b" << 1)));
}

TEST_F(SbeShardFilterTest, TopLevelArrayShardKeyGetsFiltered) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << BSON("b" << 1))),
        BSON_ARRAY(BSON("a" << BSON("b" << 2))),
        BSON_ARRAY(BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << 2))))};

    auto expected = BSON_ARRAY(BSON("a" << BSON("b" << 1)) << BSON("a" << BSON("b" << 2)));
    runTest(docs, expected, makeAlwaysPassShardFiltererFactory(BSON("a.b" << 1)));
}

TEST_F(SbeShardFilterTest, ArrayAlongBiggerShardKeyGetsFiltered) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 2 << "c" << 3 << "d" << 4)),
        BSON_ARRAY(BSON("a" << 2 << "b" << 2 << "c" << 3 << "d" << 4)),
        BSON_ARRAY(BSON("a" << BSON_ARRAY(1 << 2) << "b" << 2 << "c" << 3 << "d" << 4)),
        BSON_ARRAY(BSON("a" << 3 << "b" << 2 << "c" << 3 << "d" << 4))};

    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 2 << "c" << 3 << "d" << 4)
                               << BSON("a" << 2 << "b" << 2 << "c" << 3 << "d" << 4)
                               << BSON("a" << 3 << "b" << 2 << "c" << 3 << "d" << 4));
    runTest(
        docs, expected, makeAlwaysPassShardFiltererFactory(BSON("a" << 1 << "b" << 1 << "c" << 1)));
}

TEST_F(SbeShardFilterTest, ArrayInDottedPathKeyGetsFiltered) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON("b" << 1) << "c" << 2)),
                               BSON_ARRAY(BSON("a" << BSON("b" << 2) << "c" << 2)),
                               BSON_ARRAY(BSON("a" << BSON("b" << BSON_ARRAY(1 << 2)) << "c" << 2)),
                               BSON_ARRAY(BSON("a" << BSON("b" << 3) << "c" << 2))};

    auto expected = BSON_ARRAY(BSON("a" << BSON("b" << 1) << "c" << 2)
                               << BSON("a" << BSON("b" << 2) << "c" << 2)
                               << BSON("a" << BSON("b" << 3) << "c" << 2));
    runTest(docs, expected, makeAlwaysPassShardFiltererFactory(BSON("a.b" << 1)));
}

TEST_F(SbeShardFilterTest, ArrayAlongDeepDottedPathGetsFiltered) {
    auto docs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << BSON("e" << BSON("f" << 1))))))),
        BSON_ARRAY(BSON(
            "a" << BSON(
                "b" << BSON("c" << BSON("d" << BSON("e" << BSON("f" << BSON_ARRAY(1 << 2))))))))};
    auto expected =
        BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << BSON("e" << BSON("f" << 1)))))));
    runTest(docs, expected, makeAlwaysPassShardFiltererFactory(BSON("a.b.c.d.e.f" << 1)));
}

TEST_F(SbeShardFilterTest, MissingFieldsAreFilledCorrectly) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "c" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2 << "c" << 2)),
                                       BSON_ARRAY(BSON("c" << 2))};

    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "c" << 2)
                               << BSON("a" << 2 << "b" << 2 << "c" << 2));
    runTest(docs, expected, makeAllNullShardKeyFiltererFactory(BSON("a" << 1 << "b" << 1)));
}

TEST_F(SbeShardFilterTest, MissingFieldsDottedPathFilledCorrectly) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON("b" << 1))),
                               BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)))))};

    auto expected = BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)))));
    runTest(docs, expected, makeAllNullShardKeyFiltererFactory(BSON("a.b.c.d" << 1)));
}

TEST_F(SbeShardFilterTest, MissingFieldsAtTopDottedPathFilledCorrectly) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1)),
                               BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)))))};

    auto expected = BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)))));
    runTest(docs, expected, makeAllNullShardKeyFiltererFactory(BSON("a.b.c.d" << 1)));
}

TEST_F(SbeShardFilterTest, MissingFieldsAtBottomDottedPathFilledCorrectly) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << 1)))),
                               BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)))))};

    auto expected = BSON_ARRAY(BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)))));
    runTest(docs, expected, makeAllNullShardKeyFiltererFactory(BSON("a.b.c.d" << 1)));
}

TEST_F(SbeShardFilterTest, CoveredShardFilterPlan) {
    auto indexKeyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1);
    auto projection = BSON("a" << 1 << "c" << 1);
    auto mockedIndexKeys =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 2 << "b" << 2 << "c" << 2 << "d" << 2)),
                               BSON_ARRAY(BSON("a" << 3 << "b" << 3 << "c" << 3 << "d" << 3))};
    auto expected = BSON_ARRAY(BSON("a" << 2 << "c" << 2) << BSON("a" << 3 << "c" << 3));

    auto nss = NamespaceString{"db", "coll"};
    auto expCtx = make_intrusive<ExpressionContextForTest>(nss);
    auto emptyMatchExpression =
        unittest::assertGet(MatchExpressionParser::parse(BSONObj{}, expCtx));
    auto projectionAst = projection_ast::parse(expCtx, projection, ProjectionPolicies{});

    // Construct a PROJECTION_COVERED => SHARDING_FILTER => VIRTUAL_SCAN query solution node tree
    // where the virtual scan mocks an index scan with 'indexKeyPattern'.
    auto virtScan = std::make_unique<VirtualScanNode>(
        mockedIndexKeys, VirtualScanNode::ScanType::kIxscan, false, indexKeyPattern);
    auto shardFilter = std::make_unique<ShardingFilterNode>();
    shardFilter->children.push_back(virtScan.release());
    auto projectNode = std::make_unique<ProjectionNodeCovered>(
        std::move(shardFilter), *emptyMatchExpression, projectionAst, indexKeyPattern);

    runTest(std::move(projectNode),
            expected,
            makeAlwaysPassShardFiltererFactory(BSON("a" << 1 << "c" << 1 << "d" << 1)));
}

}  // namespace mongo
