// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/shard_filterer_mock.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/shard_filterer_factory_mock.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/pcre.h"

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
using namespace std::literals::string_view_literals;

// Default sort memory limit used when constructing SortNodes in these tests.
constexpr uint64_t kSortMaxMemoryUsageBytes = 100 * 1024 * 1024;

class SbeStageBuilderTest : public SbeStageBuilderTestFixture {
protected:
    std::unique_ptr<ShardFiltererFactoryInterface> makeAlwaysPassShardFiltererInterface() {
        return std::make_unique<ShardFiltererFactoryMock>(
            std::make_unique<ConstantFilterMock>(true, BSONObj{BSON("a" << 1)}));
    }

    /**
     * Builds a query solution with a STREAMING_GROUP node for 'groupSpec' (an $group spec) over a
     * virtual collection scan that produces 'docs' in order.
     */
    std::unique_ptr<QuerySolution> makeStreamingGroupSolution(const std::vector<BSONObj>& docs,
                                                              const BSONObj& groupSpec) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), _nss);
        VariablesParseState vps = expCtx->variablesParseState;

        std::vector<BSONArray> scanDocs;
        for (const auto& doc : docs) {
            scanDocs.push_back(BSON_ARRAY(doc));
        }
        auto scan = std::make_unique<VirtualScanNode>(
            std::move(scanDocs), VirtualScanNode::ScanType::kCollScan, false);

        auto idExpr = Expression::parseOperand(expCtx.get(), groupSpec["_id"], vps);
        std::vector<AccumulationStatement> accStmts;
        for (auto&& elem : groupSpec) {
            if (elem.fieldNameStringData() != "_id") {
                accStmts.push_back(
                    AccumulationStatement::parseAccumulationStatement(expCtx.get(), elem, vps));
            }
        }
        auto groupNode =
            std::make_unique<StreamingGroupNode>(std::move(scan),
                                                 idExpr,
                                                 std::move(accStmts),
                                                 /*shouldProduceBson=*/true,
                                                 /*nonArrayIdFields=*/OrderedPathSet{});
        return makeQuerySolution(std::move(groupNode));
    }

    /**
     * Builds a STREAMING_GROUP node with 'groupSpec' (an $group spec) over a virtual collection
     * scan that produces 'docs' in order, runs it, and checks that it produces 'expected'. The
     * caller must provide 'docs' clustered by the group key.
     */
    void assertStreamingGroupResults(const std::vector<BSONObj>& docs,
                                     const BSONObj& groupSpec,
                                     const std::vector<BSONObj>& expected,
                                     bool expectStreamingStage = true) {
        auto querySolution = makeStreamingGroupSolution(docs, groupSpec);

        auto shardFiltererInterface = makeAlwaysPassShardFiltererInterface();
        auto [resultSlots, stage, data, _] =
            buildPlanStage(std::move(querySolution), false, std::move(shardFiltererInterface));

        sbe::DebugPrintInfo debugPrintInfo{};
        const std::string plan = sbe::DebugPrinter().print(*stage, debugPrintInfo);
        ASSERT_EQ(plan.find("streaming_group ") != std::string::npos, expectStreamingStage) << plan;

        auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
        ASSERT_EQ(resultAccessors.size(), 1u);

        std::vector<BSONObj> actual;
        for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED; st = stage->getNext()) {
            auto [tag, val] = resultAccessors[0]->getViewOfValue();
            ASSERT_TRUE(tag == sbe::value::TypeTags::bsonObject);
            actual.push_back(BSONObj(sbe::value::bitcastTo<const char*>(val)).getOwned());
        }

        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_BSONOBJ_EQ(actual[i], expected[i]);
        }
    }
};

TEST_F(SbeStageBuilderTest, StreamingGroupSingleKey) {
    assertStreamingGroupResults({fromjson("{a: 1, b: 1}"),
                                 fromjson("{a: 1, b: 2}"),
                                 fromjson("{a: 2, b: 5}"),
                                 fromjson("{a: 3, b: 1}"),
                                 fromjson("{a: 3, b: 1}"),
                                 fromjson("{a: 3, b: 1}")},
                                fromjson("{_id: '$a', total: {$sum: '$b'}, n: {$sum: 1}}"),
                                {fromjson("{_id: 1, total: 3, n: 2}"),
                                 fromjson("{_id: 2, total: 5, n: 1}"),
                                 fromjson("{_id: 3, total: 3, n: 3}")});
}

// A single-reference group key has fillEmptyNull applied, so the cluster key doesn't need an
// additional fillEmptyNull and reuses the group-by slot, with no extra project.
TEST_F(SbeStageBuilderTest, StreamingGroupSingleKeyClusterSlotIsGroupBySlot) {
    auto querySolution =
        makeStreamingGroupSolution({fromjson("{a: 1}")}, fromjson("{_id: '$a', n: {$sum: 1}}"));
    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), false, makeAlwaysPassShardFiltererInterface());

    sbe::DebugPrintInfo debugPrintInfo{};
    const std::string plan = sbe::DebugPrinter().print(*stage, debugPrintInfo);

    // The stage prints as: streaming_group [keySlots] [keyOut = key] clustered [clusterKeySlots].
    static const pcre::Regex streamingGroupRegex{
        R"(streaming_group \[s(\d+)\]\s*\[s\d+ = s\d+\]\s*clustered \[s(\d+)\])"};
    const auto match = streamingGroupRegex.match(plan);
    ASSERT_TRUE(static_cast<bool>(match)) << plan;
    ASSERT_EQ(match[1], match[2]) << plan;
}

// The input of this test is deliberately not clustered by the group key. Streaming group emits a
// group whenever the key changes, so it produces three groups, where a hash group would produce
// two. This checks that a STREAMING_GROUP node really builds a streaming stage.
TEST_F(SbeStageBuilderTest, StreamingGroupEmitsGroupOnKeyChange) {
    assertStreamingGroupResults(
        {fromjson("{a: 1}"), fromjson("{a: 2}"), fromjson("{a: 1}")},
        fromjson("{_id: '$a', n: {$sum: 1}}"),
        {fromjson("{_id: 1, n: 1}"), fromjson("{_id: 2, n: 1}"), fromjson("{_id: 1, n: 1}")});
}

TEST_F(SbeStageBuilderTest, StreamingGroupUsesRegularAccumulatorsWithCompactFlag) {
    unittest::ServerParameterGuard compactFlag("featureFlagSbeCompactHashAgg", true);
    for (bool singlePurpose : {false, true}) {
        unittest::ServerParameterGuard accumulatorsFlag("featureFlagSbeAccumulators",
                                                        singlePurpose);
        // Non-clustered input distinguishes streaming from hash aggregation even when all
        // accumulators are eligible for compact aggregation.
        assertStreamingGroupResults(
            {fromjson("{a: 1, b: 2}"), fromjson("{a: 2, b: 3}"), fromjson("{a: 1, b: 4}")},
            fromjson("{_id: '$a', s: {$sum: '$b'}, p: {$push: '$b'}}"),
            {fromjson("{_id: 1, s: 2, p: [2]}"),
             fromjson("{_id: 2, s: 3, p: [3]}"),
             fromjson("{_id: 1, s: 4, p: [4]}")});
    }
}

// A constant _id doesn't use streaming group, so it falls back to the compact hash group.
TEST_F(SbeStageBuilderTest, StreamingGroupConstantKeyWithCompactFlag) {
    unittest::ServerParameterGuard compactFlag("featureFlagSbeCompactHashAgg", true);
    assertStreamingGroupResults({fromjson("{b: 2}"), fromjson("{b: 3}")},
                                fromjson("{_id: null, s: {$sum: '$b'}, p: {$push: '$b'}}"),
                                {fromjson("{_id: null, s: 5, p: [2, 3]}")},
                                /*expectStreamingStage=*/false);
}

// Accumulators that lower to more than one aggregate ($avg) and ones that don't, none of which have
// merging expressions.
TEST_F(SbeStageBuilderTest, StreamingGroupMultipleAccumulators) {
    assertStreamingGroupResults(
        {fromjson("{a: 1, b: 1}"), fromjson("{a: 1, b: 3}"), fromjson("{a: 2, b: 4}")},
        fromjson("{_id: '$a', avg: {$avg: '$b'}, mx: {$max: '$b'}}"),
        {fromjson("{_id: 1, avg: 2.0, mx: 3}"), fromjson("{_id: 2, avg: 4.0, mx: 4}")});
}

TEST_F(SbeStageBuilderTest, StreamingGroupCompoundKey) {
    assertStreamingGroupResults({fromjson("{a: 1, b: 1}"),
                                 fromjson("{a: 1, b: 1}"),
                                 fromjson("{a: 1, b: 2}"),
                                 fromjson("{a: 2, b: 1}")},
                                fromjson("{_id: {x: '$a', y: '$b'}, n: {$sum: 1}}"),
                                {fromjson("{_id: {x: 1, y: 1}, n: 2}"),
                                 fromjson("{_id: {x: 1, y: 2}, n: 1}"),
                                 fromjson("{_id: {x: 2, y: 1}, n: 1}")});
}

// Index order doesn't distinguish null from missing, so rows with null and missing 'b' can be
// interleaved. They must be clustered together, and groups within the cluster kept separate.
TEST_F(SbeStageBuilderTest, StreamingGroupInterleavedNullAndMissing) {
    assertStreamingGroupResults({fromjson("{a: 1, b: null}"),
                                 fromjson("{a: 1}"),
                                 fromjson("{a: 1, b: null}"),
                                 fromjson("{a: 1}"),
                                 fromjson("{a: 1, b: 2}"),
                                 fromjson("{a: 2}")},
                                fromjson("{_id: {x: '$a', y: '$b'}, n: {$sum: 1}}"),
                                {BSON("_id" << BSON("x" << 1 << "y" << BSONNULL) << "n" << 2),
                                 BSON("_id" << BSON("x" << 1) << "n" << 2),
                                 BSON("_id" << BSON("x" << 1 << "y" << 2) << "n" << 1),
                                 BSON("_id" << BSON("x" << 2) << "n" << 1)});
}

// Both key components read the same field, so they share a slot, which must only be passed to the
// stage once.
TEST_F(SbeStageBuilderTest, StreamingGroupDuplicateKeyPaths) {
    assertStreamingGroupResults(
        {fromjson("{a: 1}"), fromjson("{a: 1}"), fromjson("{a: 2}")},
        fromjson("{_id: {x: '$a', y: '$a'}, n: {$sum: 1}}"),
        {fromjson("{_id: {x: 1, y: 1}, n: 2}"), fromjson("{_id: {x: 2, y: 2}, n: 1}")});
}

// Both key components read the same nullable field, so they share a slot, which is projected with
// fillEmptyNull only once and shared by both cluster keys.
TEST_F(SbeStageBuilderTest, StreamingGroupDuplicateKeyPathsNullable) {
    assertStreamingGroupResults(
        {fromjson("{b: null}"), fromjson("{}"), fromjson("{b: 2}"), fromjson("{}")},
        fromjson("{_id: {x: '$b', y: '$b'}, n: {$sum: 1}}"),
        {BSON("_id" << BSON("x" << BSONNULL << "y" << BSONNULL) << "n" << 1),
         BSON("_id" << BSONObj() << "n" << 1),
         fromjson("{_id: {x: 2, y: 2}, n: 1}"),
         BSON("_id" << BSONObj() << "n" << 1)});
}

// The initializer of $maxN's 'n' is evaluated against the _id, which is an object here, so it
// differs per group. A scalar _id keeps its group-by slot, so streaming group supports this.
TEST_F(SbeStageBuilderTest, StreamingGroupVariableInitializerWithScalarId) {
    assertStreamingGroupResults(
        {fromjson("{a: {k: 1}, b: 1}"),
         fromjson("{a: {k: 1}, b: 3}"),
         fromjson("{a: {k: 1}, b: 2}"),
         fromjson("{a: {k: 0}, b: 5}"),
         fromjson("{a: {k: 0}, b: 6}")},
        fromjson("{_id: '$a', top: {$maxN: {input: '$b', n: {$add: ['$k', 1]}}}}"),
        {fromjson("{_id: {k: 1}, top: [3, 2]}"), fromjson("{_id: {k: 0}, top: [6]}")});
}

// An object _id with more keys than StreamingAggStage::kMaxClusteredGroupKeys falls back to hash
// $group instead of failing. A single group is used since hash group output order is unspecified.
TEST_F(SbeStageBuilderTest, StreamingGroupFallsBackWithTooManyGroupByKeys) {
    assertStreamingGroupResults(
        {fromjson("{a: 1, b: 1, c: 1, d: 1, e: 1}"), fromjson("{a: 1, b: 1, c: 1, d: 1, e: 1}")},
        fromjson("{_id: {a: '$a', b: '$b', c: '$c', d: '$d', e: '$e'}, n: {$sum: 1}}"),
        {fromjson("{_id: {a: 1, b: 1, c: 1, d: 1, e: 1}, n: 2}")},
        /*expectStreamingStage=*/false);
}

TEST_F(SbeStageBuilderTest, StreamingGroupEmptyInput) {
    assertStreamingGroupResults({}, fromjson("{_id: '$a', n: {$sum: 1}}"), {});
}

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

#ifdef MONGO_CONFIG_DEBUG_BUILD
using SbeStageBuilderDeathTest = SbeStageBuilderTest;

DEATH_TEST_F(SbeStageBuilderDeathTest, BlueprintRewriteBudgetExhaustionFails, "13627005") {
    unittest::ServerParameterGuard rewritesFlag{"featureFlagSbeBlueprintRewrites", true};
    unittest::ServerParameterGuard firingBudget{"internalQuerySbeBlueprintRewriteMaxFirings", 1};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1))};
    auto scan = std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);

    // Removing the unused record-id projection exhausts the single-firing budget.
    buildPlanStage(makeQuerySolution(std::move(scan)), /*hasRecordId=*/false, nullptr);
}
#else
TEST_F(SbeStageBuilderTest, BlueprintRewriteBudgetExhaustionLogsAndReturnsResults) {
    unittest::ServerParameterGuard rewritesFlag{"featureFlagSbeBlueprintRewrites", true};
    unittest::ServerParameterGuard firingBudget{"internalQuerySbeBlueprintRewriteMaxFirings", 1};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(int64_t{0} << BSON("a" << 1))};
    auto scan = std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, true);

    // Removing the unused record-id projection exhausts the single-firing budget. In release
    // builds the builder should warn, but still lower and execute the valid rewritten plan.
    unittest::LogCaptureGuard logs;
    auto [resultSlots, stage, data, _] =
        buildPlanStage(makeQuerySolution(std::move(scan)), /*hasRecordId=*/false, nullptr);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 13627006)), 1);

    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);
    auto results =
        sbe::value::TagValueOwned::fromRaw(getAllResults(stage.get(), resultAccessors[0]));
    auto expected =
        sbe::value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(BSON("a" << 1))));
    ASSERT_SBE_VALUE_EQ(results.view(), expected.view());
}
#endif

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

    void runCountScanTest() {
        createCollection(
            {fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
            BSON("a" << 1));
        // Build COUNT_SCAN node
        auto csn = std::make_unique<CountScanNode>(_nss, makeIndexEntry(BSON("a" << 1)));
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
            std::vector<AccumulationStatement>{AccumulationStatement::parseAccumulationStatement(
                _expCtx.get(), bson["count"], vps)},
            /*doingMerge=*/false,
            /*willBeMerged=*/false,
            /*shouldProduceBson=*/true);

        runTest(std::move(groupNode), BSON_ARRAY(BSON("_id" << BSONNULL << "count" << 3)));
    }

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};


TEST_F(GoldenSbeStageBuilderTest, TestCountScan) {
    runCountScanTest();
}

TEST_F(GoldenSbeStageBuilderTest, TestCountScanEfficientBoundaryMaterialization) {
    unittest::ServerParameterGuard efficientMaterialization(
        "featureFlagSbeEfficientBoundaryMaterialization", true);
    runCountScanTest();
}

class CompactGroupStageBuilderTest : public GoldenSbeStageBuilderTest {
public:
    void setUp() override {
        GoldenSbeStageBuilderTest::setUp();
        createCollection({fromjson("{_id: 0, a: 1, b: 1}"),
                          fromjson("{_id: 1, a: 1, b: NumberLong(2)}"),
                          fromjson("{_id: 2, a: 1, b: 2147483647}"),
                          fromjson("{_id: 3, a: 2, b: 2.5}"),
                          fromjson("{_id: 4, a: 2, b: NumberDecimal('1.5')}"),
                          fromjson("{_id: 5, a: 3, b: 'x'}"),
                          fromjson("{_id: 6, a: 3}")},
                         boost::none);
    }

protected:
    PlanRun runGroup(std::string_view accumulators,
                     bool compactFlag,
                     bool allowDiskUse,
                     bool merge = false) {
        unittest::ServerParameterGuard flag("featureFlagSbeCompactHashAgg", compactFlag);
        VariablesParseState vps = _expCtx->variablesParseState;
        std::vector<AccumulationStatement> statements;
        for (auto&& elem : fromjson(accumulators)) {
            statements.push_back(
                AccumulationStatement::parseAccumulationStatement(_expCtx.get(), elem, vps));
        }
        auto groupNode =
            std::make_unique<GroupNode>(getScanNode(),
                                        ExpressionFieldPath::parse(_expCtx.get(), "$a", vps),
                                        std::move(statements),
                                        /*doingMerge=*/false,
                                        /*willBeMerged=*/merge,
                                        /*shouldProduceBson=*/true);
        boost::intrusive_ptr<ExpressionContext> expCtx =
            new ExpressionContextForTest(operationContext(), _nss);
        expCtx->setAllowDiskUse(allowDiskUse);
        expCtx->setNeedsMerge(merge);
        auto run = runPlan(makeQuerySolution(std::move(groupNode)), {.expCtx = expCtx});
        run.results = sortArray(run.results.view());
        return run;
    }

    static bool isCompact(const PlanRun& run) {
        return run.explain.find("compact_group") != std::string::npos;
    }

    static std::string print(const PlanRun& run) {
        std::ostringstream os;
        os << run.results.view();
        return os.str();
    }

    void assertSameResults(const PlanRun& compact, const PlanRun& reference) {
        ASSERT_TRUE(isCompact(compact)) << compact.explain;
        ASSERT_FALSE(isCompact(reference)) << reference.explain;
        ASSERT_EQ(print(compact), print(reference));
    }

    void printGolden(const PlanRun& run) {
        _gctx->outStream() << run.explain << std::endl << print(run) << std::endl << std::endl;
    }
};

TEST_F(CompactGroupStageBuilderTest, SumAndPushUseCompactStage) {
    constexpr auto kAccs = "{p: {$push: '$b'}, s: {$sum: '$b'}, t: {$sum: '$a'}}";
    auto compact = runGroup(kAccs, true, false);
    assertSameResults(compact, runGroup(kAccs, false, false));

    _gctx->printTestHeader(unittest::GoldenTestContext::HeaderFormat::Text);
    printGolden(compact);
    _gctx->verifyOutput();
}

TEST_F(CompactGroupStageBuilderTest, PartialAccumulatorsUseCompactStage) {
    constexpr auto kAccs = "{p: {$push: '$b'}, s: {$sum: '$b'}}";
    assertSameResults(runGroup(kAccs, true, false, /*merge=*/true),
                      runGroup(kAccs, false, false, /*merge=*/true));
}

TEST_F(CompactGroupStageBuilderTest, NoAccumulatorsUsesCompactStage) {
    assertSameResults(runGroup("{}", true, false), runGroup("{}", false, false));
}

TEST_F(CompactGroupStageBuilderTest, IneligibleGroupsUseHashAgg) {
    ASSERT_FALSE(isCompact(runGroup("{s: {$sum: '$b'}}", false, false)));
    ASSERT_FALSE(isCompact(runGroup("{s: {$sum: '$b'}}", true, true)));
    ASSERT_FALSE(isCompact(runGroup("{s: {$sum: '$b'}, m: {$avg: '$b'}}", true, false)));
    ASSERT_FALSE(isCompact(runGroup("{c: {$sum: 1}}", true, false)));
    ASSERT_FALSE(isCompact(runGroup("{c: {$count: {}}}", true, false)));
}

TEST_F(GoldenSbeStageBuilderTest, TestLimitSkip) {
    createCollection(
        {fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
        BSON("a" << 1));

    auto node = std::make_unique<LimitNode>(
        std::make_unique<SkipNode>(getScanNode(), /*skip=*/1, LimitSkipParameterization::Enabled),
        /*limit=*/1,
        LimitSkipParameterization::Enabled);
    runTest(std::move(node), BSON_ARRAY(BSON("_id" << 1 << "a" << 2)));
}

TEST_F(GoldenSbeStageBuilderTest, TestSkipOnly) {
    createCollection(
        {fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}"), fromjson("{_id: 2, a: 3}")},
        BSON("a" << 1));

    auto node =
        std::make_unique<SkipNode>(getScanNode(), /*skip=*/2, LimitSkipParameterization::Enabled);
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
        /*pattern=*/BSON("a" << -1),
        /*limit=*/-1,
        LimitSkipParameterization::Disabled,
        kSortMaxMemoryUsageBytes);
    runTest(std::move(sortNode), expected);
}

TEST_F(GoldenSbeStageBuilderTest, TestCompoundSortKeepsParallelArraysCheckWithRewrites) {
    // Nothing reads the slot that checks the sort keys for parallel arrays, but dead slot
    // elimination must keep it, since the check raises an error.
    unittest::ServerParameterGuard rewritesFlag{"featureFlagSbeBlueprintRewrites", true};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 0))};
    auto expected = BSON_ARRAY(BSON("a" << 1 << "b" << 2)
                               << BSON("a" << 2 << "b" << 0) << BSON("a" << 2 << "b" << 1));

    auto sortNode = std::make_unique<SortNodeDefault>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        /*pattern=*/BSON("a" << 1 << "b" << 1),
        /*limit=*/-1,
        LimitSkipParameterization::Disabled,
        kSortMaxMemoryUsageBytes);
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
        /*pattern=*/BSON("a" << -1),
        /*limit=*/1,
        LimitSkipParameterization::Enabled,
        kSortMaxMemoryUsageBytes);
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
        /*pattern=*/BSON("a" << -1),
        /*limit=*/2,
        LimitSkipParameterization::Enabled,
        kSortMaxMemoryUsageBytes);

    auto limitSkipNode = std::make_unique<LimitNode>(
        std::make_unique<SkipNode>(
            std::move(sortNode), /*skip=*/1, LimitSkipParameterization::Enabled),
        /*limit=*/1,
        LimitSkipParameterization::Enabled);
    runTest(std::move(limitSkipNode), expected, {.limit = 1, .skip = 1});
}

std::unique_ptr<IndexScanNode> makeIdxScanNode(const NamespaceString& nss,
                                               IndexEntry indexEntry,
                                               std::string key,
                                               boost::optional<double> lowerBound,
                                               boost::optional<double> upperBound) {
    auto keyPattern = indexEntry.keyPattern;
    auto indexScanNode = std::make_unique<IndexScanNode>(nss, std::move(indexEntry));
    IndexBounds bounds{};
    if (lowerBound && upperBound) {
        OrderedIntervalList oil(key);
        oil.intervals.emplace_back(BSON("" << *lowerBound << "" << *upperBound), true, true);
        bounds.fields.emplace_back(std::move(oil));
    }
    indexScanNode->bounds = std::move(bounds);
    indexScanNode->sortSet = ProvidedSortSet{keyPattern};
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
    auto coveredSortNode = std::make_unique<SortNodeDefault>(
        makeIdxScanNode(_nss, makeIndexEntry(indexKeyPattern), "a", 1, 3),
        /*pattern=*/BSON("a" << -1),
        /*limit=*/-1,
        LimitSkipParameterization::Disabled,
        kSortMaxMemoryUsageBytes);

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
    mergeSortNode->children.push_back(std::make_unique<FetchNode>(
        makeIdxScanNode(_nss, makeIndexEntry(BSON("a" << 1)), "a", 1, 2), _nss));
    // The second branch has [{_id: 1, a: 2}, {_id: 2, a: 3}]
    mergeSortNode->children.push_back(std::make_unique<FetchNode>(
        makeIdxScanNode(_nss, makeIndexEntry(BSON("a" << 1)), "a", 2, 3), _nss));
    mergeSortNode->sort = BSON("a" << 1);
    mergeSortNode->dedup = true;

    auto node = std::make_unique<ReturnKeyNode>(std::move(mergeSortNode), std::vector<FieldPath>{});
    // MergeSort should pick {_id: 0, a: 1} in first branch since its 'a' field has smaller value.
    auto expected = BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3));
    runTest(std::move(node), expected);
}

TEST_F(GoldenSbeStageBuilderTest, TestUnusedProjectionRemovedOnlyWithRewrites) {
    auto runWithRewrites = [&](bool enabled) {
        unittest::ServerParameterGuard rewritesFlag{"featureFlagSbeBlueprintRewrites", enabled};
        auto docs = std::vector<BSONArray>{
            BSON_ARRAY(BSON("a" << 1)), BSON_ARRAY(BSON("a" << 2)), BSON_ARRAY(BSON("a" << 3))};
        auto projectNode = makeProjNode<ProjectionNodeDefault>(
            _expCtx,
            std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
            fromjson("{_id: 0, ok: {$literal: 1}}"));
        return runPlan(makeQuerySolution(std::move(projectNode)));
    };

    auto withoutRewrites = runWithRewrites(false);
    auto withRewrites = runWithRewrites(true);

    // The virtual scan projects getElement() to unpack each document, but the constant projection
    // above it never reads that slot. Only blueprint rewrites should remove this ordinary project.
    ASSERT_STRING_CONTAINS(withoutRewrites.explain, "getElement(");
    ASSERT_EQ(withRewrites.explain.find("getElement("), std::string::npos);

    auto expected = sbe::value::TagValueOwned::fromRaw(stage_builder::makeValue(
        BSON_ARRAY(BSON("ok" << 1) << BSON("ok" << 1) << BSON("ok" << 1))));
    ASSERT_SBE_VALUE_EQ(withoutRewrites.results.view(), expected.view());
    ASSERT_SBE_VALUE_EQ(withRewrites.results.view(), withoutRewrites.results.view());
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
        UnwindNode::UnwindSpec{"a"sv, true, fp});
    runTest(std::move(unwindNode), BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3)));
}

TEST_F(GoldenSbeStageBuilderTest, TestUnwindIndexPath) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << BSON_ARRAY(1 << 2 << 3)))};
    boost::optional<FieldPath> fp = FieldPath("idx");
    auto unwindNode = std::make_unique<UnwindNode>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        UnwindNode::UnwindSpec{"a"sv, true, fp});
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
        UnwindNode::UnwindSpec{"a.val"sv, true, fp});
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

TEST_F(GoldenSbeStageBuilderTest, TestUnreadReplaceRootKeepsItsCheckWithRewrites) {
    // Nothing reads the new root of the $replaceRoot, but dead slot elimination must keep it, since
    // its check raises an error when the new root is not a document.
    unittest::ServerParameterGuard rewritesFlag{"featureFlagSbeBlueprintRewrites", true};
    auto docs = std::vector<BSONArray>{BSON_ARRAY(fromjson("{a: {a: 1}}"))};
    auto replaceNode = std::make_unique<ReplaceRootNode>(
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false),
        ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "a", _expCtx->variablesParseState));
    auto projectNode = makeProjNode<ProjectionNodeDefault>(
        _expCtx, std::move(replaceNode), fromjson("{_id: 0, ok: {$literal: 1}}"));
    runTest(std::move(projectNode), BSON_ARRAY(fromjson("{ok: 1}")));
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
        _nss, makeIndexEntry(BSON("a" << "text")), textExpr.getFTSQuery().clone(), false);
    auto indexScanNode = std::make_unique<IndexScanNode>(_nss, makeIndexEntry(BSON("a" << "text")));
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
    textNode->children.push_back(std::make_unique<FetchNode>(std::move(indexScanNode), _nss));
    runTest(std::move(textNode),
            BSON_ARRAY(BSON("_id" << 0 << "a"
                                  << "this is test")
                       << BSON("_id" << 1 << "a"
                                     << "a test")));
}

}  // namespace mongo
