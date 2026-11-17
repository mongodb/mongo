// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/stages/unordered_fetch.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::stage_builder {
namespace {

class BatchedScanBuilderTest : public GoldenSbeStageBuilderTestFixture {
protected:
    auto acquireTestCollection() {
        return acquireCollectionOrView(
            operationContext(),
            CollectionOrViewAcquisitionRequest::fromOpCtx(
                operationContext(), _nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
    }

    void createTestCollection(bool clustered = false) {
        CollectionOptions options;
        if (clustered) {
            options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
        }
        createCollection({BSON("_id" << 0 << "a" << 1)}, BSON("a" << 1), options);
    }

    void createTimeseriesTestCollection() {
        CollectionOptions options;
        options.timeseries = TimeseriesOptions("t");
        options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
        createCollection({}, BSON("a" << 1), options);
        auto coll = acquireTestCollection();
        const auto& collection = coll.getCollection().getCollectionPtr();
        ASSERT_TRUE(collection->isTimeseriesCollection());
        ASSERT_TRUE(collection->isClustered());
        ASSERT_EQ(collection->getRecordStore()->keyFormat(), KeyFormat::String);
    }

    std::unique_ptr<IndexScanNode> makeScan(bool allowBatching = true) {
        auto scan = std::make_unique<IndexScanNode>(_nss, makeIndexEntry(BSON("a" << 1)));
        scan->direction = 1;
        scan->allowBatching = allowBatching;
        OrderedIntervalList oil("a");
        oil.intervals.emplace_back(BSON("" << 1 << "" << 3), true, true);
        scan->bounds.fields.push_back(std::move(oil));
        scan->computeProperties();
        return scan;
    }

    auto buildPlan(std::unique_ptr<QuerySolutionNode> root) {
        auto coll = acquireTestCollection();
        MultipleCollectionAccessor colls{coll};
        return buildPlanStage(makeQuerySolution(std::move(root)), colls, true);
    }

    std::string print(const sbe::PlanStage& stage) {
        sbe::DebugPrintInfo info{};
        return sbe::DebugPrinter{}.print(stage, info);
    }

    std::vector<int> executePlan(sbe::PlanStage& stage,
                                 PlanStageData& data,
                                 const sbe::value::SlotVector& slots) {
        auto coll = acquireTestCollection();
        MultipleCollectionAccessor colls{coll};
        attachCollectionAcquisition(colls);
        auto accessors = prepareTree(&data.env.ctx, &stage, slots);
        std::vector<int> ids;
        while (stage.getNext() == sbe::PlanState::ADVANCED) {
            ASSERT_EQ(accessors[0]->getViewOfValue().tag, sbe::value::TypeTags::RecordId);
            auto [tag, val] = accessors[1]->getViewOfValue();
            ASSERT_EQ(tag, sbe::value::TypeTags::bsonObject);
            ids.push_back(BSONObj(sbe::value::bitcastTo<const char*>(val))["_id"].Int());
        }
        stage.close();
        return ids;
    }
};

TEST_F(BatchedScanBuilderTest, BatchedFetchConsumesRecordIdBlocks) {
    createTestCollection();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(makeScan(), _nss));
    ASSERT_TRUE(dynamic_cast<sbe::UnorderedFetchStage*>(stage.get()));
    auto stats = stage->getStats(true);
    ASSERT_EQ(stats->debugInfo["maxRecordIdBatchCount"].numberLong(), 1'000'000);
    ASSERT_EQ(stats->children[0]->debugInfo["maxIndexKeyBatchCount"].numberLong(), 128);
    auto explain = print(*stage);
    ASSERT_STRING_CONTAINS(explain, "unordered_fetch");
    ASSERT_STRING_CONTAINS(explain, "batched_ixseek");
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_EQ(explain, print(*stage->clone()));
    ASSERT_EQ(executePlan(*stage, data, slots), std::vector<int>{0});
    auto clone = stage->clone();
    ASSERT_EQ(executePlan(*clone, data, slots), std::vector<int>{0});
}

TEST_F(BatchedScanBuilderTest, MultipleBatchesReturnEveryMatchingRecord) {
    unittest::ServerParameterGuard scanGuard{"internalQuerySbeBatchedIndexScanBatchSize", 3};
    unittest::ServerParameterGuard fetchGuard{"internalQuerySbeBatchedFetchBatchSize", 5};
    QueryKnobConfiguration::reset_forTest(operationContext());
    std::vector<BSONObj> docs;
    std::vector<int> expected;
    for (int i = 0; i < 300; ++i) {
        // Reverse the index order relative to insertion/RecordId order.
        docs.push_back(BSON("_id" << i << "a" << 300 - i));
        expected.push_back(i);
    }
    createCollection(docs, BSON("a" << 1));
    auto scan = makeScan();
    scan->bounds.fields[0].intervals = {Interval(BSON("" << 1 << "" << 300), true, true)};
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    auto stats = stage->getStats(true);
    ASSERT_EQ(stats->debugInfo["maxRecordIdBatchCount"].numberLong(), 5);
    ASSERT_EQ(stats->children[0]->debugInfo["maxIndexKeyBatchCount"].numberLong(), 3);
    auto ids = executePlan(*stage, data, slots);
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids, expected);
}

TEST_F(BatchedScanBuilderTest, BatchSizeKnobsAllowOneRecordPerBatch) {
    unittest::ServerParameterGuard scanGuard{"internalQuerySbeBatchedIndexScanBatchSize", 1};
    unittest::ServerParameterGuard fetchGuard{"internalQuerySbeBatchedFetchBatchSize", 1};
    QueryKnobConfiguration::reset_forTest(operationContext());
    createCollection(
        {BSON("_id" << 0 << "a" << 1), BSON("_id" << 1 << "a" << 2), BSON("_id" << 2 << "a" << 3)},
        BSON("a" << 1));
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(makeScan(), _nss));
    auto stats = stage->getStats(true);
    ASSERT_EQ(stats->debugInfo["maxRecordIdBatchCount"].numberLong(), 1);
    ASSERT_EQ(stats->children[0]->debugInfo["maxIndexKeyBatchCount"].numberLong(), 1);
    ASSERT_EQ(executePlan(*stage, data, slots), (std::vector<int>{0, 1, 2}));
}

TEST_F(BatchedScanBuilderTest, ReverseSingleIntervalBoundsUseBatchedFetch) {
    createCollection(
        {BSON("_id" << 0 << "a" << 1), BSON("_id" << 1 << "a" << 2), BSON("_id" << 2 << "a" << 3)},
        BSON("a" << 1));
    auto scan = makeScan();
    scan->direction = -1;
    scan->bounds.fields[0].intervals = {Interval(BSON("" << 3 << "" << 1), false, true)};
    scan->computeProperties();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_EQ(print(*stage).find("batched_ixseek"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_EQ(executePlan(*stage, data, slots), (std::vector<int>{0, 1}));
}

TEST_F(BatchedScanBuilderTest, ReverseSimpleRangeBoundsUseBatchedFetch) {
    createCollection(
        {BSON("_id" << 0 << "a" << 1), BSON("_id" << 1 << "a" << 2), BSON("_id" << 2 << "a" << 3)},
        BSON("a" << 1));
    auto scan = makeScan();
    scan->direction = -1;
    scan->bounds.fields.clear();
    scan->bounds.isSimpleRange = true;
    scan->bounds.startKey = BSON("" << 3);
    scan->bounds.endKey = BSON("" << 1);
    scan->bounds.boundInclusion = BoundInclusion::kIncludeEndKeyOnly;
    scan->computeProperties();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    auto explain = print(*stage);
    ASSERT_EQ(explain.find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "unordered_fetch");
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "ixseek");
    ASSERT_EQ(executePlan(*stage, data, slots), (std::vector<int>{0, 1}));
}

TEST_F(BatchedScanBuilderTest, ReverseParameterizedBoundsUseBatchedFetch) {
    createCollection(
        {BSON("_id" << 0 << "a" << 1), BSON("_id" << 1 << "a" << 2), BSON("_id" << 2 << "a" << 3)},
        BSON("a" << 1));
    auto scan = makeScan();
    scan->direction = -1;
    scan->bounds.fields[0].intervals = {Interval(BSON("" << 3 << "" << 1), false, true)};
    // IETs produce forward bounds; binding aligns them to the scan direction.
    OrderedIntervalList oil("a");
    oil.intervals.emplace_back(BSON("" << 1 << "" << 3), true, false);
    scan->iets.emplace_back(
        interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(oil));
    scan->computeProperties();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    auto explain = print(*stage);
    ASSERT_EQ(explain.find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "unordered_fetch");
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "ixseek");
    ASSERT_EQ(data.staticData->indexBoundsEvaluationInfos.size(), 1u);
    const auto& info = data.staticData->indexBoundsEvaluationInfos[0];
    ASSERT_TRUE(
        std::holds_alternative<ParameterizedIndexScanSlots::SingleIntervalPlan>(info.slots.slots));
    CanonicalQuery cq(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{std::make_unique<FindCommandRequest>(_nss)}});
    input_params::bindIndexBounds(cq, info, data.env.runtimeEnv, nullptr);
    ASSERT_EQ(executePlan(*stage, data, slots), (std::vector<int>{0, 1}));
}

TEST_F(BatchedScanBuilderTest, BatchedFetchConsumesScalarRecordIds) {
    createTestCollection();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(makeScan(false), _nss));
    ASSERT_TRUE(dynamic_cast<sbe::UnorderedFetchStage*>(stage.get()));
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "ixseek");
    ASSERT_EQ(executePlan(*stage, data, slots), std::vector<int>{0});
}

TEST_F(BatchedScanBuilderTest, ScalarRecordIdsFillMultipleFetchBatches) {
    unittest::ServerParameterGuard fetchGuard{"internalQuerySbeBatchedFetchBatchSize", 2};
    QueryKnobConfiguration::reset_forTest(operationContext());
    createCollection({BSON("_id" << 0 << "a" << 3),
                      BSON("_id" << 1 << "a" << 2),
                      BSON("_id" << 2 << "a" << 1),
                      BSON("_id" << 3 << "a" << 2),
                      BSON("_id" << 4 << "a" << 1)},
                     BSON("a" << 1));
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(makeScan(false), _nss));
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    auto ids = executePlan(*stage, data, slots);
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(BatchedScanBuilderTest, RegularFetchDoesNotBatchChildIndexScan) {
    createTestCollection();
    auto [slots, stage, data, expCtx] = buildPlan(std::make_unique<FetchNode>(makeScan(), _nss));
    auto explain = print(*stage);
    ASSERT_EQ(explain.find("batched_"), std::string::npos);
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "fetch");
}

TEST_F(BatchedScanBuilderTest, StandaloneIndexScanStaysScalar) {
    createTestCollection();
    auto [slots, stage, data, expCtx] = buildPlan(makeScan());
    ASSERT_EQ(print(*stage).find("batched_"), std::string::npos);
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
}

TEST_F(BatchedScanBuilderTest, SimpleRangeBounds) {
    createTestCollection();
    auto scan = makeScan();
    scan->bounds.fields.clear();
    scan->bounds.isSimpleRange = true;
    scan->bounds.startKey = BSON("" << 1);
    scan->bounds.endKey = BSON("" << 3);
    scan->bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_STRING_CONTAINS(print(*stage), "batched_ixseek");
    ASSERT_EQ(executePlan(*stage, data, slots), std::vector<int>{0});
}

TEST_F(BatchedScanBuilderTest, ReverseMultiIntervalBoundsUseBatchedFetch) {
    createTestCollection();
    auto scan = makeScan();
    scan->direction = -1;
    auto& intervals = scan->bounds.fields[0].intervals;
    intervals.clear();
    intervals.emplace_back(BSON("" << 5 << "" << 4), true, false);
    intervals.emplace_back(BSON("" << 2 << "" << 1), false, true);
    scan->computeProperties();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_EQ(executePlan(*stage, data, slots), std::vector<int>{0});
}

TEST_F(BatchedScanBuilderTest, PrefetchedChildStaysScalar) {
    createTestCollection();
    auto [slots, stage, data, expCtx] = buildPlan(
        std::make_unique<BatchedFetchNode>(std::make_unique<FetchNode>(makeScan(), _nss), _nss));
    ASSERT_EQ(print(*stage).find("batched_"), std::string::npos);
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_EQ(print(*stage), print(*stage->clone()));
}

TEST_F(BatchedScanBuilderTest, BatchedFetchConsumesScalarIndexDedupAndFilter) {
    createTestCollection();
    auto scan = makeScan();
    scan->shouldDedup = true;
    scan->filter = std::make_unique<AlwaysFalseMatchExpression>();
    auto fetch = std::make_unique<BatchedFetchNode>(std::move(scan), _nss);
    fetch->filter = std::make_unique<AlwaysFalseMatchExpression>();
    auto [slots, stage, data, expCtx] = buildPlan(std::move(fetch));
    auto explain = print(*stage);
    ASSERT_EQ(explain.find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "unordered_fetch");
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "unique_roaring");
    ASSERT_STRING_CONTAINS(explain, "filter");
    ASSERT_TRUE(executePlan(*stage, data, slots).empty());
}

TEST_F(BatchedScanBuilderTest, ReturnKeyUsesScalarStages) {
    createTestCollection();
    auto scan = makeScan();
    scan->addKeyMetadata = true;
    auto [slots, stage, data, expCtx] = buildPlan(std::make_unique<ReturnKeyNode>(
        std::make_unique<BatchedFetchNode>(std::move(scan), _nss), std::vector<FieldPath>{}));
    ASSERT_EQ(print(*stage).find("batched_"), std::string::npos);
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_EQ(print(*stage), print(*stage->clone()));
}

TEST_F(BatchedScanBuilderTest, ForwardedSortKeysUseScalarStages) {
    createTestCollection();
    auto merge = std::make_unique<MergeSortNode>();
    merge->sort = BSON("a" << 1);
    merge->dedup = false;
    for (size_t i = 0; i < 2; ++i) {
        // A batched fetch can reorder records, so explicitly sort each branch before merging.
        merge->children.push_back(
            std::make_unique<SortNodeDefault>(std::make_unique<BatchedFetchNode>(makeScan(), _nss),
                                              BSON("a" << 1),
                                              0,
                                              LimitSkipParameterization::Disabled,
                                              100 * 1024 * 1024));
    }
    auto [slots, stage, data, expCtx] = buildPlan(std::move(merge));
    auto explain = print(*stage);
    ASSERT_EQ(explain.find("unordered_fetch"), std::string::npos);
    ASSERT_EQ(explain.find("batched_ix"), std::string::npos);
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_EQ(explain, print(*stage->clone()));
}

TEST_F(BatchedScanBuilderTest, ParameterizedBatchedBoundsAreRegisteredAndBound) {
    createTestCollection();
    auto scan = makeScan();
    scan->iets.emplace_back(
        interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(
            scan->bounds.fields[0]));
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_EQ(data.staticData->indexBoundsEvaluationInfos.size(), 1u);
    const auto& info = data.staticData->indexBoundsEvaluationInfos[0];
    auto boundsSlots = std::get<ParameterizedIndexScanSlots::SingleIntervalPlan>(info.slots.slots);
    ASSERT_TRUE(data.env->isSlotRegistered(boundsSlots.lowKey));
    ASSERT_TRUE(data.env->isSlotRegistered(boundsSlots.highKey));
    CanonicalQuery cq(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{std::make_unique<FindCommandRequest>(_nss)}});
    input_params::bindIndexBounds(cq, info, data.env.runtimeEnv, nullptr);
    ASSERT_EQ(data.env->getAccessor(boundsSlots.lowKey)->getViewOfValue().tag,
              sbe::value::TypeTags::keyString);
    ASSERT_EQ(data.env->getAccessor(boundsSlots.highKey)->getViewOfValue().tag,
              sbe::value::TypeTags::keyString);
    ASSERT_EQ(executePlan(*stage, data, slots), std::vector<int>{0});
    auto reboundInfo = info;
    OrderedIntervalList oil("a");
    oil.intervals.emplace_back(BSON("" << 9 << "" << 11), true, true);
    reboundInfo.iets[0] =
        interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(oil);
    input_params::bindIndexBounds(cq, reboundInfo, data.env.runtimeEnv);
    ASSERT_TRUE(executePlan(*stage->clone(), data, slots).empty());
    oil.intervals.clear();
    reboundInfo.iets[0] =
        interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(oil);
    input_params::bindIndexBounds(cq, reboundInfo, data.env.runtimeEnv);
    ASSERT_TRUE(executePlan(*stage->clone(), data, slots).empty());
    ASSERT_STRING_CONTAINS(print(*stage), "batched_ixseek");
}

TEST_F(BatchedScanBuilderTest, ParameterizedMultiIntervalBoundsUseBatchedFetch) {
    createTestCollection();
    auto scan = makeScan();
    scan->bounds.fields[0].intervals.emplace_back(BSON("" << 5 << "" << 7), true, true);
    scan->iets.emplace_back(
        interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(
            scan->bounds.fields[0]));
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_EQ(data.staticData->indexBoundsEvaluationInfos.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ParameterizedIndexScanSlots::GenericPlan>(
        data.staticData->indexBoundsEvaluationInfos[0].slots.slots));
    CanonicalQuery cq(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{std::make_unique<FindCommandRequest>(_nss)}});
    input_params::bindIndexBounds(
        cq, data.staticData->indexBoundsEvaluationInfos[0], data.env.runtimeEnv, nullptr);
    ASSERT_EQ(executePlan(*stage, data, slots), std::vector<int>{0});
}

TEST_F(BatchedScanBuilderTest, StringRecordIdsFallBackToScalarStages) {
    createTestCollection(true);
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(makeScan(), _nss));
    auto explain = print(*stage);
    ASSERT_EQ(explain.find("unordered_fetch"), std::string::npos);
    ASSERT_EQ(explain.find("batched_ix"), std::string::npos);
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "fetch");
}

TEST_F(BatchedScanBuilderTest, BatchingDisabledLeavesScalarStagesUnchanged) {
    createTestCollection();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<FetchNode>(makeScan(false), _nss));
    ASSERT_EQ(print(*stage).find("batched_"), std::string::npos);
}

TEST_F(BatchedScanBuilderTest, TimeseriesCollectionFallsBackToScalarStages) {
    createTimeseriesTestCollection();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(makeScan(), _nss));
    auto explain = print(*stage);
    ASSERT_EQ(explain.find("unordered_fetch"), std::string::npos);
    ASSERT_EQ(explain.find("batched_ix"), std::string::npos);
    ASSERT_EQ(explain.find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(explain, "fetch");
}

TEST_F(BatchedScanBuilderTest, TimeseriesCollectionWithParameterizedBoundsStaysScalar) {
    createTimeseriesTestCollection();
    auto scan = makeScan();
    scan->iets.emplace_back(
        interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(
            scan->bounds.fields[0]));
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    ASSERT_EQ(data.staticData->indexBoundsEvaluationInfos.size(), 1u);
    ASSERT_EQ(print(*stage).find("unordered_fetch"), std::string::npos);
}

TEST_F(BatchedScanBuilderTest, BatchedFetchConsumesScalarIndexDedup) {
    createCollection({BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2 << 3)),
                      BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 3))},
                     BSON("a" << 1));
    auto scan = makeScan();
    scan->shouldDedup = true;
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "unique_roaring");
    ASSERT_EQ(executePlan(*stage, data, slots), (std::vector<int>{0, 1}));
}

TEST_F(BatchedScanBuilderTest, BatchedFetchConsumesScalarIndexFilter) {
    createTestCollection();
    auto scan = makeScan();
    scan->filter = std::make_unique<AlwaysFalseMatchExpression>();
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(scan), _nss));
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "filter");
    ASSERT_TRUE(executePlan(*stage, data, slots).empty());
}

TEST_F(BatchedScanBuilderTest, FetchFilterRunsAfterScalarOutputOfBatchedPair) {
    createTestCollection();
    auto fetch = std::make_unique<BatchedFetchNode>(makeScan(), _nss);
    fetch->filter = std::make_unique<AlwaysFalseMatchExpression>();
    auto [slots, stage, data, expCtx] = buildPlan(std::move(fetch));
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_STRING_CONTAINS(print(*stage), "batched_ixseek");
    ASSERT_STRING_CONTAINS(print(*stage), "filter");
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_TRUE(executePlan(*stage, data, slots).empty());
}

TEST_F(BatchedScanBuilderTest, FetchFilterReadsExtractedFieldSlots) {
    createCollection({BSON("_id" << 0 << "a" << 1 << "b" << 4),
                      BSON("_id" << 1 << "a" << 2 << "b" << 7),
                      BSON("_id" << 2 << "a" << 3 << "b" << 9)},
                     BSON("a" << 1));
    auto fetch = std::make_unique<BatchedFetchNode>(makeScan(), _nss);
    fetch->filter = std::make_unique<GTMatchExpression>(std::string_view{"b"}, Value(5));
    auto [slots, stage, data, expCtx] = buildPlan(std::move(fetch));
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_EQ(executePlan(*stage, data, slots), (std::vector<int>{1, 2}));
}

TEST_F(BatchedScanBuilderTest, BatchedFetchConsumesInterveningScalarStage) {
    createCollection(
        {BSON("_id" << 0 << "a" << 3), BSON("_id" << 1 << "a" << 2), BSON("_id" << 2 << "a" << 1)},
        BSON("a" << 1));
    auto limit = std::make_unique<LimitNode>(makeScan(), 2, LimitSkipParameterization::Disabled);
    auto [slots, stage, data, expCtx] =
        buildPlan(std::make_unique<BatchedFetchNode>(std::move(limit), _nss));
    ASSERT_EQ(print(*stage).find("batched_ix"), std::string::npos);
    ASSERT_STRING_CONTAINS(print(*stage), "unordered_fetch");
    ASSERT_STRING_CONTAINS(print(*stage), "limit");
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_EQ(executePlan(*stage, data, slots), (std::vector<int>{1, 2}));
}

TEST_F(BatchedScanBuilderTest, StandaloneParameterizedIndexScanStaysScalar) {
    createTestCollection();
    auto scan = makeScan();
    scan->iets.emplace_back(
        interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(
            scan->bounds.fields[0]));
    auto [slots, stage, data, expCtx] = buildPlan(std::move(scan));
    ASSERT_EQ(print(*stage).find("batched_"), std::string::npos);
    ASSERT_EQ(print(*stage).find("block_to_row"), std::string::npos);
    ASSERT_EQ(data.staticData->indexBoundsEvaluationInfos.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ParameterizedIndexScanSlots::SingleIntervalPlan>(
        data.staticData->indexBoundsEvaluationInfos[0].slots.slots));
}

}  // namespace
}  // namespace mongo::stage_builder
