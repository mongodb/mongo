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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/shard_filterer_mock.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/shard_filterer_factory_mock.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

class SbeStageBuilderTest : public SbeStageBuilderTestFixture {
protected:
    std::unique_ptr<ShardFiltererFactoryInterface> makeAlwaysPassShardFiltererInterface() {
        return std::make_unique<ShardFiltererFactoryMock>(
            std::make_unique<ConstantFilterMock>(true, BSONObj{BSON("a" << 1)}));
    }
};

TEST_F(SbeStageBuilderTest, TestVirtualScan) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced.
    auto virtScan =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);

    // Make a QuerySolution from the root virtual scan node.
    auto querySolution = makeQuerySolution(std::move(virtScan));
    ASSERT_EQ(querySolution->root()->nodeId(), 1);

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), true, std::move(shardFiltererInterface));
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the recordIDs are what we expect.
        auto [tag, val] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tag == sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(index, sbe::value::bitcastTo<int64_t>(val));

        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[1]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));
        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

TEST_F(SbeStageBuilderTest, TestLimitOneVirtualScan) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a root limit node that takes ownership of a
    // VirtualScanNode.
    auto virtScan =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);
    auto limitNode =
        std::make_unique<LimitNode>(std::move(virtScan), 1, LimitSkipParameterization::Disabled);

    // Make a QuerySolution from the root limitNode.
    auto querySolution = makeQuerySolution(std::move(limitNode));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), true, std::move(shardFiltererInterface));

    // Prepare the sbe::PlanStage for execution.
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the recordIDs are what we expect.
        auto [tag, val] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tag == sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(index, sbe::value::bitcastTo<int64_t>(val));

        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[1]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));

        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 1);
}

TEST_F(SbeStageBuilderTest, VirtualCollScanWithoutRecordId) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a root limit node that takes ownership of a
    // VirtualScanNode.
    auto virtScan =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);
    auto querySolution = makeQuerySolution(std::move(virtScan));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), false, std::move(shardFiltererInterface));

    // Prepare the sbe::PlanStage for execution.
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));

        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

TEST_F(SbeStageBuilderTest, VirtualIndexScan) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced.
    auto virtScan = std::make_unique<VirtualScanNode>(
        docs, VirtualScanNode::ScanType::kIxscan, true, BSON("a" << 1 << "b" << 1));
    auto querySolution = makeQuerySolution(std::move(virtScan));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), true, std::move(shardFiltererInterface));
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 2u);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the recordIDs are what we expect.
        auto [tag, val] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tag == sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(index, sbe::value::bitcastTo<int64_t>(val));

        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[1]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));
        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

TEST_F(SbeStageBuilderTest, VirtualIndexScanWithoutRecordId) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced.
    auto virtScan = std::make_unique<VirtualScanNode>(
        docs, VirtualScanNode::ScanType::kIxscan, false, BSON("a" << 1 << "b" << 1));
    auto querySolution = makeQuerySolution(std::move(virtScan));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), false, std::move(shardFiltererInterface));
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));
        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

TEST_F(SbeStageBuilderTest, VirtualScanWithFilter) {
    auto filteredDocs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                               BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                               BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    BSONObj query = fromjson("{b: 2}");
    auto filter = uassertStatusOK(MatchExpressionParser::parse(
        query, make_intrusive<ExpressionContextForTest>(operationContext(), _nss)));

    std::vector<BSONArray> allDocs = filteredDocs;
    allDocs.insert(allDocs.begin() + 1, BSON_ARRAY(BSON("a" << 2 << "b" << 1)));
    allDocs.insert(allDocs.end(), BSON_ARRAY(BSON("a" << 4 << "b" << 1)));

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced and filtered, according to the provided filter.
    auto virtScan = std::make_unique<VirtualScanNode>(
        std::move(allDocs), VirtualScanNode::ScanType::kCollScan, false);
    virtScan->filter = std::move(filter);
    // Make a QuerySolution from the root virtual scan node.
    auto querySolution = makeQuerySolution(std::move(virtScan));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), false, std::move(shardFiltererInterface));
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);

    int64_t index = 0;
    for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
        // Assert that the document produced from the stage is what we expect.
        auto [tagDoc, valDoc] = resultAccessors[0]->getViewOfValue();
        ASSERT_TRUE(tagDoc == sbe::value::TypeTags::bsonObject);
        auto bo = BSONObj(sbe::value::bitcastTo<const char*>(valDoc));
        ASSERT_BSONOBJ_EQ(bo, BSON("a" << ++index << "b" << 2));
    }
    ASSERT_EQ(index, 3);
}

class GoldenSbeStageBuilderTest : public GoldenSbeStageBuilderTestFixture {
public:
    void setUp() override {
        SbeStageBuilderTestFixture::setUp();
        _expCtx = new ExpressionContextForTest();
    }

    void tearDown() override {
        _expCtx = nullptr;
        SbeStageBuilderTestFixture::tearDown();
    }

    std::unique_ptr<CollectionScanNode> getScanNode() {
        auto node = std::make_unique<CollectionScanNode>();
        node->nss = _nss;
        return node;
    }

    std::unique_ptr<ShardFiltererFactoryInterface> makeAlwaysPassShardFiltererInterface() {
        return std::make_unique<ShardFiltererFactoryMock>(
            std::make_unique<ConstantFilterMock>(true, BSONObj{BSON("a" << 1)}));
    }

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

IndexEntry makeIndexEntry(BSONObj keyPattern) {
    return {keyPattern,
            IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
            IndexConfig::kLatestIndexVersion,
            false /* multiKey */,
            {{}, {}} /* multiKeyPaths */,
            {} /* multikeyPathSet */,
            false /* sp */,
            false /* unq */,
            CoreIndexInfo::Identifier(DBClientBase::genIndexName(keyPattern)),
            nullptr /* fe */,
            {} /* io */,
            nullptr /* ci */,
            nullptr /* wildcardProjection */};
}

TEST_F(GoldenSbeStageBuilderTest, TestCountScan) {
    createCollection(
        {fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
        BSON("a" << 1));
    // Build COUNT_SCAN node
    auto csn = std::make_unique<CountScanNode>(makeIndexEntry(BSON("a" << 1)));
    csn->startKey = BSON("a" << BSONType::minKey);
    csn->startKeyInclusive = false;
    csn->endKey = BSON("a" << BSONType::maxKey);
    csn->endKeyInclusive = false;
    // Build GROUP node
    auto bson = fromjson("{count: {$count: {}}}");
    VariablesParseState vps = _expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(_expCtx.get(), "$_id", vps);
    auto groupNode = std::make_unique<GroupNode>(
        std::move(csn),
        groupByExpression,
        std::vector<AccumulationStatement>{
            AccumulationStatement::parseAccumulationStatement(_expCtx.get(), bson["count"], vps)},
        false /*doingMerge*/,
        false /*willBeMerged*/,
        true /*shouldProduceBson*/);

    runTest(std::move(groupNode), BSON_ARRAY(BSON("_id" << BSONNULL << "count" << 3)));
}

TEST_F(GoldenSbeStageBuilderTest, TestLimitSkip) {
    createCollection(
        {fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
        BSON("a" << 1));

    auto node = std::make_unique<LimitNode>(
        std::make_unique<SkipNode>(getScanNode(), 1 /* skip */, LimitSkipParameterization::Enabled),
        1 /* limit */,
        LimitSkipParameterization::Enabled);
    runTest(std::move(node), BSON_ARRAY(BSON("_id" << 1 << "a" << 2)));
}

TEST_F(GoldenSbeStageBuilderTest, TestSkipOnly) {
    createCollection(
        {fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
        BSON("a" << 1));

    auto node =
        std::make_unique<SkipNode>(getScanNode(), 2 /* skip */, LimitSkipParameterization::Enabled);
    runTest(std::move(node), BSON_ARRAY(BSON("_id" << 2 << "a" << 3)));
}

TEST_F(GoldenSbeStageBuilderTest, TestSort) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto expected = BSON_ARRAY(BSON("a" << 3 << "b" << 2)
                               << BSON("a" << 2 << "b" << 2) << BSON("a" << 1 << "b" << 2));

    // Build a collection scan node for uncovered sort.
    auto sortNode = std::make_unique<SortNodeDefault>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        BSON("a" << -1) /* pattern */,
        -1 /* limit */,
        LimitSkipParameterization::Disabled);
    runTest(std::move(sortNode), expected);
}

TEST_F(GoldenSbeStageBuilderTest, TestSortLimit) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto expected = BSON_ARRAY(BSON("a" << 3 << "b" << 2));

    // Build a collection scan node for uncovered sort.
    auto sortNode = std::make_unique<SortNodeDefault>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        BSON("a" << -1) /* pattern */,
        1 /* limit */,
        LimitSkipParameterization::Enabled);
    runTest(std::move(sortNode), expected, {.limit = 1});
}

TEST_F(GoldenSbeStageBuilderTest, TestSortLimitSkip) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto expected = BSON_ARRAY(BSON("a" << 2 << "b" << 2));

    // Build a collection scan node for uncovered sort.
    auto sortNode = std::make_unique<SortNodeDefault>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        BSON("a" << -1) /* pattern */,
        2 /* limit */,
        LimitSkipParameterization::Enabled);

    auto limitSkipNode = std::make_unique<LimitNode>(
        std::make_unique<SkipNode>(
            std::move(sortNode), 1 /* skip */, LimitSkipParameterization::Enabled),
        1 /* limit */,
        LimitSkipParameterization::Enabled);
    runTest(std::move(limitSkipNode), expected, {.limit = 1, .skip = 1});
}

std::unique_ptr<IndexScanNode> makeIdxScanNode(BSONObj idxPattern,
                                               std::string key,
                                               boost::optional<double> lowerBound,
                                               boost::optional<double> upperBound) {
    auto indexScanNode = std::make_unique<IndexScanNode>(makeIndexEntry(idxPattern));
    IndexBounds bounds{};
    if (lowerBound && upperBound) {
        OrderedIntervalList oil(key);
        oil.intervals.emplace_back(BSON("" << *lowerBound << "" << *upperBound), true, true);
        bounds.fields.emplace_back(std::move(oil));
    }
    indexScanNode->bounds = std::move(bounds);
    indexScanNode->sortSet = ProvidedSortSet{idxPattern};
    return indexScanNode;
}

template <typename T, typename... Args>
std::unique_ptr<T> makeProjNode(boost::intrusive_ptr<ExpressionContext> expCtx,
                                std::unique_ptr<QuerySolutionNode> child,
                                BSONObj projection,
                                Args... args) {
    auto emptyMatchExpression =
        unittest::assertGet(MatchExpressionParser::parse(BSONObj{}, expCtx));
    auto projectionAst = projection_ast::parseAndAnalyze(expCtx, projection, ProjectionPolicies{});

    return std::make_unique<T>(
        std::move(child), emptyMatchExpression.get(), projectionAst, args...);
}

TEST_F(GoldenSbeStageBuilderTest, TestSortCovered) {
    auto indexKeyPattern = BSON("a" << 1);
    createCollection(
        {fromjson("{_id: 0, a: 1, b: 2}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
        indexKeyPattern);

    // Build an index scan node for covered sort.
    auto coveredSortNode =
        std::make_unique<SortNodeDefault>(makeIdxScanNode(indexKeyPattern, "a", 1, 3),
                                          BSON("a" << -1) /* pattern */,
                                          -1 /* limit */,
                                          LimitSkipParameterization::Disabled);

    // Build covered projection so that sort stage doesn't need to return whole document and becomes
    // covered sort.
    auto projection = BSON("a" << 1 << "_id" << 0);
    auto projectNode = makeProjNode<ProjectionNodeCovered>(
        _expCtx, std::move(coveredSortNode), projection, indexKeyPattern);

    auto expected = BSON_ARRAY(BSON("a" << 3) << BSON("a" << 2) << BSON("a" << 1));
    runTest(std::move(projectNode), expected);
}

TEST_F(GoldenSbeStageBuilderTest, TestMergeSort) {
    createCollection(
        {fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
        BSON("a" << 1));

    auto mergeSortNode = std::make_unique<MergeSortNode>();
    // The first branch has [{_id: 0, a: 1}, {_id: 1, a: 2}]
    mergeSortNode->children.push_back(
        std::make_unique<FetchNode>(makeIdxScanNode(BSON("a" << 1), "a", 1, 2)));
    // The second branch has [{_id: 1, a: 2}, {_id: 2, a: 3}]
    mergeSortNode->children.push_back(
        std::make_unique<FetchNode>(makeIdxScanNode(BSON("a" << 1), "a", 2, 3)));
    mergeSortNode->sort = BSON("a" << 1);
    mergeSortNode->dedup = true;

    auto node = std::make_unique<ReturnKeyNode>(std::move(mergeSortNode), std::vector<FieldPath>{});
    // MergeSort should pick {_id: 0, a: 1} in first branch since its 'a' field has smaller value.
    auto expected = BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3));
    runTest(std::move(node), expected);
}

TEST_F(GoldenSbeStageBuilderTest, TestMatch) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto matchNode = std::make_unique<MatchNode>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        std::make_unique<AlwaysFalseMatchExpression>());

    auto projectNode = makeProjNode<ProjectionNodeDefault>(
        _expCtx, std::move(matchNode), BSON("a" << 1 << "_id" << 0));
    runTest(std::move(projectNode), BSONArray());
}

TEST_F(GoldenSbeStageBuilderTest, TestOr) {
    // Create a dummy collection so that we can test collection metadata in the stage builder, even
    // though this test only uses virtualscan stage.
    createCollection({fromjson("{_id: 0}")}, boost::none);

    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};
    auto orNode = std::make_unique<OrNode>();
    orNode->children.emplace_back(makeProjNode<ProjectionNodeDefault>(
        _expCtx,
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true),
        BSON("a" << 1)));
    orNode->children.emplace_back(makeProjNode<ProjectionNodeDefault>(
        _expCtx,
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true),
        BSON("b" << 1)));

    BSONObj query = fromjson("{a: 2}");
    orNode->filter = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));
    orNode->dedup = true;
    auto projectNode = makeProjNode<ProjectionNodeDefault>(_expCtx,
                                                           std::move(orNode),
                                                           BSON("a" << 1 << "c"
                                                                    << "b"));
    runTest(std::move(projectNode),
            BSON_ARRAY(BSON("a" << 2 << "c"
                                << "b")));
}

TEST_F(GoldenSbeStageBuilderTest, TestUnwind) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON_ARRAY(1 << 2 << 3)))};
    boost::optional<FieldPath> fp = boost::none;
    auto unwindNode = std::make_unique<UnwindNode>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        UnwindNode::UnwindSpec{"a"_sd, true, fp});
    runTest(std::move(unwindNode), BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3)));
}

TEST_F(GoldenSbeStageBuilderTest, TestUnwindIndexPath) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON_ARRAY(1 << 2 << 3)))};
    boost::optional<FieldPath> fp = FieldPath("idx");
    auto unwindNode = std::make_unique<UnwindNode>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        UnwindNode::UnwindSpec{"a"_sd, true, fp});
    runTest(std::move(unwindNode),
            BSON_ARRAY(BSON("a" << 1 << "idx" << 0)
                       << BSON("a" << 2 << "idx" << 1) << BSON("a" << 3 << "idx" << 2)));
}

TEST_F(GoldenSbeStageBuilderTest, TestUnwindIndexPathConflict) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON("val" << BSON_ARRAY(1 << 2 << 3))))};
    boost::optional<FieldPath> fp = FieldPath("a.idx");
    auto unwindNode = std::make_unique<UnwindNode>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        UnwindNode::UnwindSpec{"a.val"_sd, true, fp});
    runTest(std::move(unwindNode),
            BSON_ARRAY(BSON("a" << BSON("val" << 1 << "idx" << 0))
                       << BSON("a" << BSON("val" << 2 << "idx" << 1))
                       << BSON("a" << BSON("val" << 3 << "idx" << 2))));
}

TEST_F(GoldenSbeStageBuilderTest, TestReplaceRoot) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON("a" << 1) << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << BSON("a" << 2) << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << BSON("a" << 3) << "b" << 2))};
    auto replaceNode = std::make_unique<ReplaceRootNode>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "a", _expCtx->variablesParseState));
    runTest(std::move(replaceNode), BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3)));
}

TEST_F(GoldenSbeStageBuilderTest, TestShardFilter) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 2)
                               << BSON("a" << 2 << "b" << 2) << BSON("a" << 3 << "b" << 2));

    auto sfn = std::make_unique<ShardingFilterNode>();
    sfn->children.push_back(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false));
    runTest(
        std::move(sfn), expected, {.shardFilterInterface = makeAlwaysPassShardFiltererInterface()});
}

TEST_F(GoldenSbeStageBuilderTest, TestShardFilterCovered) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    auto expected = BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3));

    auto sfn = std::make_unique<ShardingFilterNode>();
    sfn->children.push_back(std::make_unique<VirtualScanNode>(
        docs, VirtualScanNode::ScanType::kIxscan, false, BSON("a" << 1)));

    // Build covered projection so that sort stage doesn't need to return whole document and becomes
    // covered sort.
    auto projection = BSON("a" << 1 << "_id" << 0);
    auto projectNode =
        makeProjNode<ProjectionNodeCovered>(_expCtx, std::move(sfn), projection, BSON("a" << 1));

    runTest(std::move(projectNode),
            expected,
            {.shardFilterInterface = makeAlwaysPassShardFiltererInterface()});
}

TEST_F(GoldenSbeStageBuilderTest, TestTextMatch) {
    createCollection({fromjson("{_id: 0, a: 'this is test'}"),
                      fromjson("{_id: 1, a: 'a test'}"),
                      fromjson("{_id: 2, a: 'hello'}")},
                     BSON("a" << "text"));
    TextMatchExpression textExpr(operationContext(),
                                 _nss,
                                 TextMatchExpressionBase::TextParams{.query = "test",
                                                                     .language = "english",
                                                                     .caseSensitive = true});
    auto textNode = std::make_unique<TextMatchNode>(
        makeIndexEntry(BSON("a" << "text")), textExpr.getFTSQuery().clone(), false);
    auto indexScanNode = std::make_unique<IndexScanNode>(makeIndexEntry(BSON("a" << "text")));
    IndexBounds bounds{};
    OrderedIntervalList oil("a");
    oil.intervals.emplace_back(BSON("" << "a"
                                       << ""
                                       << "z"),
                               true,
                               true);
    bounds.fields.emplace_back(std::move(oil));
    indexScanNode->bounds = std::move(bounds);
    indexScanNode->sortSet = ProvidedSortSet{BSON("a" << "text")};
    textNode->children.push_back(std::make_unique<FetchNode>(std::move(indexScanNode)));
    runTest(std::move(textNode),
            BSON_ARRAY(BSON("_id" << 0 << "a"
                                  << "this is test")
                       << BSON("_id" << 1 << "a"
                                     << "a test")));
}

class SearchSbeStageBuilderTest : public GoldenSbeStageBuilderTest {
public:
    void setUp() override {
        GoldenSbeStageBuilderTest::setUp();
        _gctx->validateOnClose(true);
        _gctx->printTestHeader(GoldenTestContext::HeaderFormat::Text);
    }

    void tearDown() override {
        GoldenSbeStageBuilderTest::tearDown();
    }

    void runSearchTest(std::unique_ptr<QuerySolutionNode> root,
                       const mongo::BSONArray& expectedValue) {
        auto findCommand = std::make_unique<FindCommandRequest>(_nss);
        auto cq = std::make_unique<CanonicalQuery>(
            CanonicalQueryParams{.expCtx = _expCtx,
                                 .parsedFind = ParsedFindCommandParams{std::move(findCommand)},
                                 .isSearchQuery = true});
        auto* searchNode = static_cast<SearchNode*>(root.get());

        // Mock a DocumentSourceSearch and set the cqPipeline in CanonicalQuery.
        BSONObjBuilder builder;
        builder.append("mongotQuery", searchNode->searchQuery);
        if (searchNode->limit) {
            builder.append("limit", *searchNode->limit);
        }
        auto elem = BSON("" << builder.obj());
        cq->setCqPipeline({DocumentSourceSearch::createFromBson(elem.firstElement(), _expCtx)},
                          false);

        auto querySolution = makeQuerySolution(std::move(root));
        // Translate the QuerySolution tree to an sbe::PlanStage.
        auto localColl =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), _nss, AcquisitionPrerequisites::kRead),
                              MODE_IS);
        MultipleCollectionAccessor colls(localColl);
        _expCtx->setUUID(localColl.uuid());

        auto [resultSlots, stage, data, _] = buildPlanStage(
            std::move(querySolution), colls, false /*hasRecordId*/, {.expCtx = _expCtx});

        boost::optional<Lock::GlobalLock> globalLock;
        if (!shard_role_details::getLocker(operationContext())->isLocked()) {
            globalLock.emplace(operationContext(), MODE_IS);
        }

        // Mock the mongot response.
        auto remoteCursors = std::make_unique<RemoteCursorMap>();
        remoteCursors->insert(
            {kCursorIdResult, mockTaskExecutorCursor(operationContext(), 0, kSearchResultArray)});
        remoteCursors->insert({kCursorIdStoredSource,
                               mockTaskExecutorCursor(operationContext(), 0, kResultStoredSource)});

        auto name = Variables::getBuiltinVariableName(Variables::kSearchMetaId);
        data.env->resetSlot(data.env->getSlot(name),
                            sbe::value::TypeTags::bsonObject,
                            sbe::value::bitcastFrom<const char*>(kSearchMeta.objdata()),
                            false /* owned */);
        ASSERT_NE(getYieldPolicy(), nullptr);
        stage_builder::prepareSlotBasedExecutableTree(operationContext(),
                                                      stage.get(),
                                                      &data,
                                                      *cq,
                                                      colls,
                                                      getYieldPolicy(),
                                                      false,
                                                      remoteCursors.get());
        ASSERT_EQ(resultSlots.size(), 1u);
        auto resultAccessor = stage->getAccessor(data.env.ctx, resultSlots[0]);

        // Print the stage explain output and verify.
        _gctx->outStream() << data.debugString() << std::endl;
        auto explain = sbe::DebugPrinter().print(*stage.get());
        _gctx->outStream() << replaceUuid(explain, localColl.uuid());
        _gctx->outStream() << std::endl;

        stage->open(false);
        // Execute the plan to verify explain output is correct.
        auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessor);
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedValue);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

        ASSERT_TRUE(
            PlanStageTestFixture::valueEquals(resultsTag, resultsVal, expectedTag, expectedVal))
            << "expected: " << std::make_pair(expectedTag, expectedVal)
            << " but got: " << std::make_pair(resultsTag, resultsVal);
    }

protected:
    std::unique_ptr<executor::TaskExecutorCursor> mockTaskExecutorCursor(
        OperationContext* opCtx, CursorId cursorId, const BSONArray& firstBatch) {
        auto networkInterface = std::make_unique<executor::NetworkInterfaceMock>();
        auto testExecutor = executor::makeThreadPoolTestExecutor(std::move(networkInterface));
        executor::RemoteCommandRequest req = executor::RemoteCommandRequest();
        req.opCtx = opCtx;

        std::vector<BSONObj> batchVec;
        for (const auto& ele : firstBatch) {
            batchVec.push_back(ele.Obj());
        }

        executor::TaskExecutorCursorOptions opts(/*pinConnection*/ gPinTaskExecCursorConns.load(),
                                                 /*batchSize*/ boost::none,
                                                 /*preFetchNextBatch*/ false);
        return std::make_unique<executor::TaskExecutorCursor>(
            testExecutor,
            nullptr /* underlyingExec */,
            CursorResponse{NamespaceString::kEmpty, cursorId, batchVec},
            req,
            std::move(opts));
    }

    const size_t kCursorIdResult = 0;
    const size_t kCursorIdStoredSource = 1;
    const BSONObj kSearchMeta = BSON("count" << 2);
    const BSONArray kSearchResultArray = BSON_ARRAY(fromjson(R"(
{
    "_id" : 0,
    "metaA" : 0,
    "a" : 1
})") << fromjson(R"(
{
    "_id" : 1,
    "metaA" : 2,
    "a" : 2
})"));

    const BSONArray kResultStoredSource = BSON_ARRAY(fromjson(R"(
{
    "storedSource" : {
        "a" : 1
    },
    "metaA" : 10
})") << fromjson(R"(
{
    "storedSource" : {
        "a" : 2
    },
    "metaA" : 20
})"));
};

TEST_F(SearchSbeStageBuilderTest, TestSearch) {
    createCollection({fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}")}, boost::none);
    // Test search meta.
    {
        _gctx->outStream() << "SearchMeta Test" << std::endl;
        auto node =
            std::make_unique<SearchNode>(true /* isSearchMeta */,
                                         BSON("query" << "test"
                                                      << "path"
                                                      << "a"),
                                         boost::none /* limit */,
                                         boost::none /* sortSpec */,
                                         kCursorIdResult /* remoteCursorId */,
                                         BSON("SEARCH_META" << kSearchMeta) /* remoteCursorVars*/);
        runSearchTest(std::move(node), BSON_ARRAY(kSearchMeta));
    }
    // Test non-stored_source case.
    {
        _gctx->outStream() << "Search NonStoredSource Test" << std::endl;
        auto node = std::make_unique<SearchNode>(false /* isSearchMeta */,
                                                 BSON("query" << "test"
                                                              << "path"
                                                              << "a"),
                                                 boost::none /* limit */,
                                                 boost::none /* sortSpec */,
                                                 kCursorIdResult /* remoteCursorId */,
                                                 boost::none /* remoteCursorVars*/);
        runSearchTest(std::move(node),
                      BSON_ARRAY(BSON("_id" << 0 << "a" << 1) << BSON("_id" << 1 << "a" << 2)));
    }
    // Test stored_source case with limit.
    {
        _gctx->outStream() << "Search NonStoredSource Test" << std::endl;
        auto node = std::make_unique<SearchNode>(false /* isSearchMeta */,
                                                 BSON("query" << "test"
                                                              << "path"
                                                              << "a"
                                                              << "returnStoredSource" << true),
                                                 1 /* limit */,
                                                 boost::none /* sortSpec */,
                                                 kCursorIdStoredSource /* remoteCursorId */,
                                                 boost::none /* remoteCursorVars*/);
        runSearchTest(std::move(node), BSON_ARRAY(BSON("a" << 1)));
    }
}
}  // namespace mongo
