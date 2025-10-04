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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/shard_filterer_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/shard_filterer_factory_mock.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace mongo {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

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
        shardFilter->children.push_back(std::move(virtScan));
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
        auto [resultSlots, stage, data, _] =
            buildPlanStage(std::move(querySolution), false, std::move(shardFiltererFactory));

        // Prepare the sbe::PlanStage for execution and collect all results.
        auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
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
    auto projection = BSON("a" << 1 << "c" << 1 << "_id" << 0);
    auto mockedIndexKeys =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 2 << "b" << 2 << "c" << 2 << "d" << 2)),
                               BSON_ARRAY(BSON("a" << 3 << "b" << 3 << "c" << 3 << "d" << 3))};
    auto expected = BSON_ARRAY(BSON("a" << 2 << "c" << 2) << BSON("a" << 3 << "c" << 3));

    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    auto expCtx = make_intrusive<ExpressionContextForTest>(nss);
    auto emptyMatchExpression =
        unittest::assertGet(MatchExpressionParser::parse(BSONObj{}, expCtx));
    auto projectionAst = projection_ast::parseAndAnalyze(expCtx, projection, ProjectionPolicies{});

    // Construct a PROJECTION_COVERED => SHARDING_FILTER => VIRTUAL_SCAN query solution node tree
    // where the virtual scan mocks an index scan with 'indexKeyPattern'.
    auto virtScan = std::make_unique<VirtualScanNode>(
        mockedIndexKeys, VirtualScanNode::ScanType::kIxscan, false, indexKeyPattern);
    auto shardFilter = std::make_unique<ShardingFilterNode>();
    shardFilter->children.push_back(std::move(virtScan));
    auto projectNode = std::make_unique<ProjectionNodeCovered>(
        std::move(shardFilter), emptyMatchExpression.get(), projectionAst, indexKeyPattern);

    runTest(std::move(projectNode),
            expected,
            makeAlwaysPassShardFiltererFactory(BSON("a" << 1 << "c" << 1 << "d" << 1)));
}

class SbeShardKeyExpressionTest : public sbe::EExpressionTestFixture {
public:
    void runShardKeyExpressionTest(BSONObj shardKeyPatternDefinition,
                                   std::vector<BSONObj> documents) {
        std::vector<sbe::MatchPath> shardKeyPaths;
        std::vector<bool> shardKeyHashed;

        stage_builder::PlanStageSlots slots;
        std::vector<sbe::value::ViewOfValueAccessor> slotAccessors;
        slotAccessors.reserve(shardKeyPatternDefinition.nFields());

        for (const auto& shardKeyElt : shardKeyPatternDefinition) {
            shardKeyPaths.emplace_back(shardKeyElt.fieldNameStringData());
            shardKeyHashed.push_back(ShardKeyPattern::isHashedPatternEl(shardKeyElt));

            slotAccessors.emplace_back();
            slots.set(std::make_pair(stage_builder::PlanStageSlots::kField,
                                     shardKeyPaths.back().getPart(0)),
                      stage_builder::SbSlot{bindAccessor(&slotAccessors.back())});
        }

        auto shardKeyExpression = stage_builder::makeShardKeyForPersistedDocuments(
                                      _state, shardKeyPaths, shardKeyHashed, slots)
                                      .lower(_state);
        auto compiledShardKey = compileExpression(*shardKeyExpression);

        ShardKeyPattern shardKeyPattern{shardKeyPatternDefinition};

        for (const BSONObj& document : documents) {
            for (size_t index = 0; index < shardKeyPaths.size(); ++index) {
                auto path = shardKeyPaths[index].getPart(0);
                BSONElement elem = document.getField(path);
                const auto& [tag, val] = sbe::bson::convertFrom<true>(elem);
                slotAccessors[index].reset(tag, val);
            }
            BSONObj classicShardKey = shardKeyPattern.extractShardKeyFromDoc(document);

            const auto& [tag, val] = runCompiledExpression(compiledShardKey.get());
            sbe::value::ValueGuard guard{tag, val};
            ASSERT_EQ(sbe::value::TypeTags::bsonObject, tag);

            BSONObj sbeShardKey{sbe::value::bitcastTo<const char*>(val)};
            ASSERT_BSONOBJ_EQ(classicShardKey, sbeShardKey);
        }
    }
};

TEST_F(SbeShardKeyExpressionTest, SingleShardKeyPattern) {
    runShardKeyExpressionTest(
        BSON("a" << 1),
        {BSON("a" << 10 << "b" << 20), BSON("b" << 20), BSON("a" << BSON("b" << 20))});
}

TEST_F(SbeShardKeyExpressionTest, NestedShardKeyPattern) {
    runShardKeyExpressionTest(BSON("a.b" << 1 << "c" << 1),
                              {BSON("a" << BSON("b" << 10) << "c" << 20),
                               BSON("a" << BSON("b" << 10)),
                               BSON("c" << 20),
                               BSON("a" << 10 << "c" << 20),
                               BSON("a" << BSON("b" << BSON("z" << 100)) << "c" << 20)});
}

TEST_F(SbeShardKeyExpressionTest, HashedShardKeyPattern) {
    runShardKeyExpressionTest(
        BSON("a" << "hashed"),
        {BSON("a" << 10 << "b" << 20), BSON("b" << 20), BSON("a" << BSON("b" << 20))});
}

}  // namespace mongo
