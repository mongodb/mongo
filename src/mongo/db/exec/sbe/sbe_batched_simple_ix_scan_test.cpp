// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/batched_simple_ix_scan.h"
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
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace mongo::sbe {
namespace {

class BatchedSimpleIndexScanStageTest : public PlanStageTestFixture {
public:
    // The test fixture creates an index with integer keys in the range [1, kNumKeys], where each
    // entry with key _i_ has a record id 10 * _i_.
    static constexpr int kNumKeys = 12;

    static int64_t ridForKey(int a) {
        return a * 10;
    }

    using Batches = std::vector<std::vector<int64_t>>;

    /**
     * Scans the index from 'lowKey' to 'highKey' (both inclusive) with the given batch size and
     * checks that the record ids produced by the calls to getNext() match the 'expected' values,
     * one inner vector per call. The scan runs three times: on the freshly opened stage, again
     * after closing and reopening it, and again on a clone of it.
     */
    void assertScan(size_t maxIndexKeyBatchCount,
                    int lowKey,
                    int highKey,
                    const Batches& expected) {
        auto nss = NamespaceString::createNamespaceString_forTest(
            "testdb.sbe_batched_simple_ix_scan_test");
        auto opCtx = operationContext();

        ASSERT_OK(storageInterface()->createCollection(opCtx, nss, CollectionOptions()));
        ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
            opCtx, nss, {BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1))}));

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto entry = autoColl->getIndexCatalog()->findIndexByName(opCtx, "a_1");
            auto sdi = entry->accessMethod()->asSortedData()->getSortedDataInterface();
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

            WriteUnitOfWork wuow(opCtx);
            for (int a = 1; a <= kNumKeys; ++a) {
                key_string::Builder key(sdi->getKeyStringVersion(), sdi->getOrdering());
                key.appendNumberInt(a);
                key.appendRecordId(RecordId{ridForKey(a)});
                ASSERT_OK(std::get<Status>(sdi->insert(opCtx, ru, key.getValueCopy(), false)));
            }
            wuow.commit();
        }

        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
        MultipleCollectionAccessor mca(coll);
        attachCollectionAcquisition(mca);

        auto makeSeekKey = [&](int a, key_string::Discriminator discriminator) {
            auto entry = coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(opCtx, "a_1");
            auto sdi = entry->accessMethod()->asSortedData()->getSortedDataInterface();
            key_string::Builder kb(sdi->getKeyStringVersion(), sdi->getOrdering(), discriminator);
            kb.appendNumberInt(a);
            kb.appendDiscriminator(discriminator);
            auto [tag, val] = value::makeKeyString(kb.getValueCopy());
            return makeE<EConstant>(tag, val);
        };

        auto ctx = makeCompileCtx();
        auto ridSlot = generateSlotId();
        auto stage = makeS<BatchedSimpleIndexScanStage>(
            coll.uuid(),
            nss.dbName(),
            "a_1",
            maxIndexKeyBatchCount,
            ridSlot,
            /*snapshotIdSlot=*/boost::none,
            /*indexIdentSlot=*/boost::none,
            makeSeekKey(lowKey, key_string::Discriminator::kExclusiveBefore),
            makeSeekKey(highKey, key_string::Discriminator::kExclusiveAfter),
            getYieldPolicy(),
            kEmptyPlanNodeId);

        auto accessor = prepareTree(ctx.get(), stage.get(), ridSlot);

        auto runScan = [&](PlanStage* root, value::SlotAccessor* ridAccessor) {
            Batches batches;
            while (root->getNext() == PlanState::ADVANCED) {
                ASSERT_LT(batches.size(), static_cast<size_t>(kNumKeys));
                auto [tag, val] = ridAccessor->getViewOfValue();
                if (maxIndexKeyBatchCount > 1) {
                    ASSERT_EQ(tag, value::TypeTags::valueBlock);
                    auto* block =
                        dynamic_cast<value::Int64RecordIdBlock*>(value::getValueBlock(val));
                    ASSERT(block);
                    batches.push_back(block->getVector());
                } else {
                    ASSERT_EQ(tag, value::TypeTags::RecordId);
                    batches.push_back({value::getRecordIdView(val)->getLong()});
                }
            }
            return batches;
        };

        // First scan, on the freshly opened stage.
        ASSERT_EQ(runScan(stage.get(), accessor), expected);

        // Second scan, after closing and reopening the stage.
        stage->close();
        stage->open(false);
        ASSERT_EQ(runScan(stage.get(), accessor), expected);
        stage->close();

        // Third scan, on a clone of the stage.
        auto cloneCtx = makeCompileCtx();
        auto clone = stage->clone();
        auto cloneAccessor = prepareTree(cloneCtx.get(), clone.get(), ridSlot);
        ASSERT_EQ(runScan(clone.get(), cloneAccessor), expected);
        clone->close();
    }
};

using Batches = BatchedSimpleIndexScanStageTest::Batches;

TEST_F(BatchedSimpleIndexScanStageTest, BatchCountOneNoMatches) {
    assertScan(/*maxIndexKeyBatchCount=*/1, 100, 200, Batches{});
}

TEST_F(BatchedSimpleIndexScanStageTest, BatchCountOneMultipleMatches) {
    assertScan(/*maxIndexKeyBatchCount=*/1, 2, 5, (Batches{{20}, {30}, {40}, {50}}));
}

TEST_F(BatchedSimpleIndexScanStageTest, BatchCountFiveNoMatches) {
    assertScan(/*maxIndexKeyBatchCount=*/5, 100, 200, Batches{});
}

TEST_F(BatchedSimpleIndexScanStageTest, BatchCountFiveFewerMatchesThanBatchSize) {
    // Keys 2 through 4 are 3 keys: a single partial batch.
    assertScan(/*maxIndexKeyBatchCount=*/5, 2, 4, (Batches{{20, 30, 40}}));
}

TEST_F(BatchedSimpleIndexScanStageTest, BatchCountFiveMatchesEvenMultipleOfBatchSize) {
    // Keys 2 through 11 are 10 keys: exactly two full batches.
    assertScan(/*maxIndexKeyBatchCount=*/5,
               2,
               11,
               (Batches{{20, 30, 40, 50, 60}, {70, 80, 90, 100, 110}}));
}

TEST_F(BatchedSimpleIndexScanStageTest, BatchCountFiveShortFinalBatch) {
    // Keys 2 through 12 are 11 keys: two full batches and a partial batch of one.
    assertScan(/*maxIndexKeyBatchCount=*/5,
               2,
               12,
               (Batches{{20, 30, 40, 50, 60}, {70, 80, 90, 100, 110}, {120}}));
}
}  // namespace
}  // namespace mongo::sbe
