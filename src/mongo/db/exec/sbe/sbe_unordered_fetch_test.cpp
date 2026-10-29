// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/unordered_fetch.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mongo::sbe {
namespace {
using namespace std::literals::string_view_literals;

// Each input to the 'UnorderedFetchStage' is either a single record id or a block of record ids.
using RecordIdInput = std::variant<int64_t, std::vector<int64_t>>;
using Batches = std::vector<std::vector<int64_t>>;

/**
 * Simulates a 'BatchedSimpleIndexScanStage' by producing a predetermined sequence of record ids in
 * its output slot. Each call to getNext() produces either a single RecordId or an
 * 'Int64RecordIdBlock', depending on the corresponding input. Like the real index scan, the output
 * accessor owns the value, which gets freed on the next call to getNext(), so a parent that uses a
 * block after calling getNext() again reads freed memory.
 */
class MockRecordIdSourceStage final : public PlanStage {
public:
    MockRecordIdSourceStage(std::vector<RecordIdInput> inputs,
                            value::SlotId outSlot,
                            PlanNodeId nodeId)
        : PlanStage("mock_rid_source"sv, nullptr, nodeId, false),
          _inputs(std::move(inputs)),
          _outSlot(outSlot) {}

    std::unique_ptr<PlanStage> clone() const final {
        return std::make_unique<MockRecordIdSourceStage>(_inputs, _outSlot, _commonStats.nodeId);
    }

    void prepare(CompileCtx& ctx) final {}

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final {
        if (slot == _outSlot) {
            return &_outAccessor;
        }
        return ctx.getAccessor(slot);
    }

    void open(bool reOpen) final {
        _commonStats.opens++;
        _position = 0;
        _outAccessor.reset();
    }

    PlanState getNext() final {
        // Free the previous output before producing the next one.
        _outAccessor.reset();

        if (_position == _inputs.size()) {
            return trackPlanState(PlanState::IS_EOF);
        }

        const auto& input = _inputs[_position++];
        if (const auto* rid = std::get_if<int64_t>(&input)) {
            _outAccessor.reset(value::TagValueOwned::fromRaw(value::makeNewRecordId(*rid)));
        } else {
            auto block =
                std::make_unique<value::Int64RecordIdBlock>(std::get<std::vector<int64_t>>(input));
            _outAccessor.reset(value::TagValueOwned::fromRaw(
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(block.release())));
        }

        return trackPlanState(PlanState::ADVANCED);
    }

    void close() final {
        trackClose();
        _outAccessor.reset();
    }

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final {
        return std::make_unique<PlanStageStats>(_commonStats);
    }

    const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    void doDebugPrint(std::vector<DebugPrinter::Block>&, DebugPrintInfo&) const final {}

    size_t estimateCompileTimeSize() const final {
        return sizeof(*this);
    }

protected:
    void doSaveState() final {}
    void doRestoreState() final {}

private:
    const std::vector<RecordIdInput> _inputs;
    const value::SlotId _outSlot;

    value::OwnedValueAccessor _outAccessor;
    size_t _position{0};
};

class UnorderedFetchStageTest : public PlanStageTestFixture {
public:
    // The test fixture creates a collection with documents {_id: i, a: 100 * i} for i in the range
    // [1, kNumDocs], where each document with _id i has record id i.
    static constexpr int kNumDocs = 20;

    static int64_t fieldValueForRid(int64_t rid) {
        return rid * 100;
    }

    /**
     * Runs the fetch stage 'root' to completion and groups the fetched record ids by the batch that
     * they were part of, using the stage's count of batches to detect when a new batch begins. The
     * 'accessors' are for the stage's record id, result, and "a" field slots, in that order.
     */
    static Batches runFetch(PlanStage* root, const std::vector<value::SlotAccessor*>& accessors) {
        auto numBatches = [&] {
            return root->getStats(true /* includeDebugInfo */)
                ->debugInfo["batches"]
                .safeNumberLong();
        };

        Batches batches;
        const auto initialBatches = numBatches();
        while (root->getNext() == PlanState::ADVANCED) {
            auto [ridTag, ridVal] = accessors[0]->getViewOfValue();
            ASSERT_EQ(ridTag, value::TypeTags::RecordId);
            const auto rid = value::getRecordIdView(ridVal)->getLong();

            auto [resultTag, resultVal] = accessors[1]->getViewOfValue();
            ASSERT_EQ(resultTag, value::TypeTags::bsonObject);
            BSONObj result(value::bitcastTo<const char*>(resultVal));
            ASSERT_EQ(result["_id"].numberLong(), rid);

            auto [fieldTag, fieldVal] = accessors[2]->getViewOfValue();
            ASSERT_EQ(fieldTag, value::TypeTags::NumberInt64);
            ASSERT_EQ(value::bitcastTo<int64_t>(fieldVal), fieldValueForRid(rid));

            const auto batchIndex = numBatches() - initialBatches - 1;
            ASSERT_GTE(batchIndex, 0);
            ASSERT_LTE(batchIndex, static_cast<long long>(batches.size()));
            if (batchIndex == static_cast<long long>(batches.size())) {
                batches.emplace_back();
            }
            batches.back().push_back(rid);
        }
        return batches;
    }

    /**
     * Fetches the documents for the record ids produced by a mock child stage that emits the
     * given 'inputs' and checks that the fetched record ids match the 'expected' values, with one
     * inner vector per batch. The fetch runs three times: on the freshly opened stage, again after
     * closing and reopening it, and again on a clone of it.
     */
    void assertFetch(size_t maxRecordIdBatchCount,
                     std::vector<RecordIdInput> inputs,
                     const Batches& expected) {
        auto nss =
            NamespaceString::createNamespaceString_forTest("testdb.sbe_unordered_fetch_test");
        auto opCtx = operationContext();

        ASSERT_OK(storageInterface()->createCollection(opCtx, nss, CollectionOptions()));
        {
            std::vector<BSONObj> docs;
            for (int i = 1; i <= kNumDocs; ++i) {
                docs.push_back(BSON("_id" << i << "a" << fieldValueForRid(i)));
            }

            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            ASSERT_OK(Helpers::insert(opCtx, *autoColl, docs));
            wuow.commit();
        }

        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
        MultipleCollectionAccessor mca(coll);
        attachCollectionAcquisition(mca);

        auto seekSlot = generateSlotId();
        auto resultSlot = generateSlotId();
        auto recordIdSlot = generateSlotId();
        auto fieldSlot = generateSlotId();
        auto stage = makeS<UnorderedFetchStage>(
            makeS<MockRecordIdSourceStage>(std::move(inputs), seekSlot, kEmptyPlanNodeId),
            coll.uuid(),
            nss.dbName(),
            seekSlot,
            resultSlot,
            recordIdSlot,
            std::vector<std::string>{"a"},
            value::SlotVector{fieldSlot},
            maxRecordIdBatchCount,
            getYieldPolicy(),
            kEmptyPlanNodeId);

        auto ctx = makeCompileCtx();
        auto accessors = prepareTree(
            ctx.get(), stage.get(), value::SlotVector{recordIdSlot, resultSlot, fieldSlot});

        // First fetch, on the freshly opened stage.
        ASSERT_EQ(runFetch(stage.get(), accessors), expected);

        // Second fetch, after closing and reopening the stage.
        stage->close();
        stage->open(false);
        ASSERT_EQ(runFetch(stage.get(), accessors), expected);
        stage->close();

        // Third fetch, on a clone of the stage.
        auto cloneCtx = makeCompileCtx();
        auto clone = stage->clone();
        auto cloneAccessors = prepareTree(
            cloneCtx.get(), clone.get(), value::SlotVector{recordIdSlot, resultSlot, fieldSlot});
        ASSERT_EQ(runFetch(clone.get(), cloneAccessors), expected);
        clone->close();
    }
};

TEST_F(UnorderedFetchStageTest, NoInput) {
    assertFetch(/*maxRecordIdBatchCount=*/4, {}, Batches{});
}

TEST_F(UnorderedFetchStageTest, SingleRecordIdsFewerThanBatchCount) {
    assertFetch(
        /*maxRecordIdBatchCount=*/4, {int64_t{7}, int64_t{3}, int64_t{5}}, Batches{{3, 5, 7}});
}

TEST_F(UnorderedFetchStageTest, SingleRecordIdsEvenMultipleOfBatchCount) {
    assertFetch(/*maxRecordIdBatchCount=*/4,
                {int64_t{9},
                 int64_t{2},
                 int64_t{14},
                 int64_t{1},
                 int64_t{20},
                 int64_t{6},
                 int64_t{11},
                 int64_t{8}},
                Batches{{1, 2, 9, 14}, {6, 8, 11, 20}});
}

TEST_F(UnorderedFetchStageTest, SingleRecordIdsShortFinalBatch) {
    assertFetch(
        /*maxRecordIdBatchCount=*/3,
        {int64_t{9}, int64_t{2}, int64_t{14}, int64_t{1}, int64_t{20}, int64_t{6}, int64_t{11}},
        Batches{{2, 9, 14}, {1, 6, 20}, {11}});
}

TEST_F(UnorderedFetchStageTest, SingleRecordIdsBatchCountOne) {
    assertFetch(/*maxRecordIdBatchCount=*/1,
                {int64_t{9}, int64_t{2}, int64_t{14}},
                Batches{{9}, {2}, {14}});
}

TEST_F(UnorderedFetchStageTest, BlockSmallerThanBatchCount) {
    assertFetch(
        /*maxRecordIdBatchCount=*/8, {std::vector<int64_t>{12, 4, 17}}, Batches{{4, 12, 17}});
}

TEST_F(UnorderedFetchStageTest, BatchCountEqualToBlockSize) {
    assertFetch(/*maxRecordIdBatchCount=*/3,
                {std::vector<int64_t>{12, 4, 17},
                 std::vector<int64_t>{3, 19, 8},
                 std::vector<int64_t>{10, 1, 15}},
                Batches{{4, 12, 17}, {3, 8, 19}, {1, 10, 15}});
}

TEST_F(UnorderedFetchStageTest, BatchCountEvenMultipleOfBlockSize) {
    // Each batch consists of exactly two blocks.
    assertFetch(/*maxRecordIdBatchCount=*/6,
                {std::vector<int64_t>{12, 4, 17},
                 std::vector<int64_t>{3, 19, 8},
                 std::vector<int64_t>{10, 1, 15},
                 std::vector<int64_t>{6, 20, 2}},
                Batches{{3, 4, 8, 12, 17, 19}, {1, 2, 6, 10, 15, 20}});
}

TEST_F(UnorderedFetchStageTest, BatchCountEvenMultipleOfBlockSizeShortFinalBlock) {
    // The final block is smaller than the others, as when an index scan reaches the end of its
    // bounds partway through a block, so the final batch is not full.
    assertFetch(/*maxRecordIdBatchCount=*/6,
                {std::vector<int64_t>{12, 4, 17},
                 std::vector<int64_t>{3, 19, 8},
                 std::vector<int64_t>{10, 1, 15},
                 std::vector<int64_t>{6}},
                Batches{{3, 4, 8, 12, 17, 19}, {1, 6, 10, 15}});
}

TEST_F(UnorderedFetchStageTest, BatchCountNotMultipleOfBlockSize) {
    // Blocks of 4 record ids split across batches of 6.
    assertFetch(/*maxRecordIdBatchCount=*/6,
                {std::vector<int64_t>{12, 4, 17, 9},
                 std::vector<int64_t>{3, 19, 8, 14},
                 std::vector<int64_t>{10, 1, 15, 5},
                 std::vector<int64_t>{6, 20, 2, 18}},
                Batches{{3, 4, 9, 12, 17, 19}, {1, 5, 8, 10, 14, 15}, {2, 6, 18, 20}});
}

TEST_F(UnorderedFetchStageTest, BatchCountSmallerThanBlockSize) {
    // Each block of 5 record ids spans multiple batches of 2.
    assertFetch(/*maxRecordIdBatchCount=*/2,
                {std::vector<int64_t>{12, 4, 17, 9, 3}, std::vector<int64_t>{19, 8, 14, 10, 1}},
                Batches{{4, 12}, {9, 17}, {3, 19}, {8, 14}, {1, 10}});
}

TEST_F(UnorderedFetchStageTest, BatchCountOneWithBlocks) {
    assertFetch(/*maxRecordIdBatchCount=*/1,
                {std::vector<int64_t>{12, 4}, std::vector<int64_t>{17}},
                Batches{{12}, {4}, {17}});
}

TEST_F(UnorderedFetchStageTest, InterleavedSingleRecordIdsAndBlocksEvenBatches) {
    // Each batch consists of one block of 3 and one single record id.
    assertFetch(/*maxRecordIdBatchCount=*/4,
                {std::vector<int64_t>{12, 4, 17},
                 int64_t{2},
                 int64_t{19},
                 std::vector<int64_t>{3, 11, 8},
                 std::vector<int64_t>{10, 1, 15},
                 int64_t{5}},
                Batches{{2, 4, 12, 17}, {3, 8, 11, 19}, {1, 5, 10, 15}});
}

TEST_F(UnorderedFetchStageTest, InterleavedSingleRecordIdsAndBlocksSplitBlocks) {
    // Blocks get split across batch boundaries with single record ids in between them.
    assertFetch(/*maxRecordIdBatchCount=*/4,
                {int64_t{20},
                 std::vector<int64_t>{12, 4, 17, 9, 3},
                 int64_t{2},
                 int64_t{19},
                 std::vector<int64_t>{7, 11, 8},
                 int64_t{5},
                 std::vector<int64_t>{10, 1}},
                Batches{{4, 12, 17, 20}, {2, 3, 9, 19}, {5, 7, 8, 11}, {1, 10}});
}
}  // namespace
}  // namespace mongo::sbe
