// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/record_store_slicer.h"

#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace collection_validation {
namespace {

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::SizeIs;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.slicer");

class RecordStoreSlicerTest : public CatalogTestFixture {
protected:
    void createCollection() {
        CollectionOptions options;
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, options))
            << "Failed to create collection";
    }

    void createClusteredCollection() {
        CollectionOptions options;
        options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, options))
            << "Failed to create collection";
    }

    /**
     * Inserts 'count' documents. A non-clustered collection assigns them consecutive long RecordIds
     * starting at 1, so the resulting RecordId range is [1, count].
     */
    void insertDocuments(int count) {
        for (int i = 1; i <= count; ++i) {
            ASSERT_OK(storageInterface()->insertDocument(operationContext(),
                                                         kNss,
                                                         {BSON("_id" << i), Timestamp()},
                                                         repl::OpTime::kUninitializedTerm));
        }
    }

    std::vector<RecordId> pivots(size_t sliceCount) {
        auto* opCtx = operationContext();
        const AutoGetCollection coll(opCtx, kNss, MODE_IS);
        return computeSlicePivots(opCtx,
                                  *shard_role_details::getRecoveryUnit(opCtx),
                                  *coll->getRecordStore(),
                                  sliceCount);
    }
};

TEST_F(RecordStoreSlicerTest, EvenlyDivisibleRangeIsStridedEvenly) {
    createCollection();
    // A [1, 101] RecordId range over 4 slices strides by 25.
    insertDocuments(101);
    EXPECT_THAT(pivots(4), ElementsAre(RecordId(1), RecordId(26), RecordId(51), RecordId(76)));
}

TEST_F(RecordStoreSlicerTest, StrideFloorsAndFinalSliceAbsorbsRemainder) {
    createCollection();
    // A [1, 11] range over 4 slices floors the stride to 2, so the pivots stay well inside the
    // range and the caller's open-ended final slice covers everything from 7 onwards.
    insertDocuments(11);
    EXPECT_THAT(pivots(4), ElementsAre(RecordId(1), RecordId(3), RecordId(5), RecordId(7)));
}

TEST_F(RecordStoreSlicerTest, PivotsStayStrictlyBelowLastRecordId) {
    createCollection();
    insertDocuments(1000);
    // No pivot may land on or past the last RecordId, which would produce an empty trailing slice.
    for (size_t sliceCount : {2, 3, 7, 16, 64}) {
        const auto slicePivots = pivots(sliceCount);
        EXPECT_THAT(slicePivots, SizeIs(testing::Le(sliceCount))) << "sliceCount: " << sliceCount;
        EXPECT_EQ(RecordId(1), *slicePivots.begin()) << "sliceCount: " << sliceCount;
        EXPECT_LT(*slicePivots.rbegin(), RecordId(1000)) << "sliceCount: " << sliceCount;
    }
}

TEST_F(RecordStoreSlicerTest, RangeNarrowerThanSliceCountYieldsSingleSlice) {
    createCollection();
    // A stride that floors to zero cannot separate the slices, so fall back to one slice.
    insertDocuments(4);
    EXPECT_THAT(pivots(8), ElementsAre(RecordId(1)));
}

TEST_F(RecordStoreSlicerTest, SingleRecordYieldsSingleSlice) {
    createCollection();
    insertDocuments(1);
    EXPECT_THAT(pivots(4), ElementsAre(RecordId(1)));
}

TEST_F(RecordStoreSlicerTest, OneSliceRequestedYieldsSingleSlice) {
    createCollection();
    insertDocuments(100);
    EXPECT_THAT(pivots(1), ElementsAre(RecordId(1)));
    EXPECT_THAT(pivots(0), ElementsAre(RecordId(1)));
}

TEST_F(RecordStoreSlicerTest, EmptyRecordStoreYieldsNoPivots) {
    createCollection();
    // The forward cursor comes back empty, which is how the slicer detects an empty record store:
    // there is nothing to traverse, so it describes no slices at all.
    EXPECT_THAT(pivots(4), IsEmpty());
}

TEST_F(RecordStoreSlicerTest, StringRecordIdsYieldSingleSlice) {
    createClusteredCollection();
    // Clustered collections use string-formatted RecordIds, which have no stride arithmetic, so the
    // whole record store needs to traverses as a single slice.
    insertDocuments(100);
    const auto slicePivots = pivots(8);
    EXPECT_THAT(slicePivots, SizeIs(1));
    EXPECT_TRUE(slicePivots.begin()->isStr());
}

/**
 * This reproduces how validate_adaptor.cpp turns pivots into slices for testing.
 */
class RecordStoreSliceCoverageTest : public RecordStoreSlicerTest {
protected:
    /**
     * Deletes the documents with the given '_id's, leaving holes in the RecordId range so that
     * pivots can land on RecordIds that no record holds.
     */
    void deleteDocuments(const std::vector<int>& ids) {
        for (int id : ids) {
            ASSERT_OK(storageInterface()
                          ->deleteById(operationContext(), kNss, BSON("_id" << id).firstElement())
                          .getStatus());
        }
    }

    std::vector<RecordId> serialScan() {
        auto* opCtx = operationContext();
        const AutoGetCollection coll(opCtx, kNss, MODE_IS);
        auto cursor = coll->getRecordStore()->getCursor(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), /*forward=*/true);
        std::vector<RecordId> visited;
        for (auto record = cursor->next(); record; record = cursor->next()) {
            visited.push_back(record->id);
        }
        return visited;
    }

    /**
     * Traverses every slice implied by 'slicePivots' in order and returns the RecordIds visited,
     * concatenated. Duplicates are deliberately preserved so the comparison against the serial scan
     * fails.
     */
    std::vector<RecordId> slicedScan(const std::vector<RecordId>& slicePivots) {
        auto* opCtx = operationContext();
        const AutoGetCollection coll(opCtx, kNss, MODE_IS);
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

        std::vector<RecordId> visited;
        for (size_t i = 0; i < slicePivots.size(); ++i) {
            const RecordId endRecordId =
                (i + 1 < slicePivots.size()) ? slicePivots[i + 1] : RecordId();
            auto cursor = coll->getRecordStore()->getCursor(opCtx, ru, /*forward=*/true);
            for (auto record =
                     cursor->seek(slicePivots[i], SeekableRecordCursor::BoundInclusion::kInclude);
                 record && (endRecordId.isNull() || record->id < endRecordId);
                 record = cursor->next()) {
                visited.push_back(record->id);
            }
        }
        return visited;
    }

    /**
     * Asserts that slicing into 'sliceCount' slices visits exactly what a serial scan visits, in
     * the same order.
     */
    void assertSlicedScanMatchesSerialScan(size_t sliceCount) {
        const auto slicePivots = pivots(sliceCount);
        EXPECT_THAT(slicedScan(slicePivots), ElementsAreArray(serialScan()))
            << "sliceCount: " << sliceCount;
    }
};

TEST_F(RecordStoreSliceCoverageTest, DenseRangeIsCoveredExactlyOnce) {
    createCollection();
    insertDocuments(1000);
    for (size_t sliceCount : {1, 2, 3, 7, 16, 64, 999, 1000, 1001}) {
        assertSlicedScanMatchesSerialScan(sliceCount);
    }
}

TEST_F(RecordStoreSliceCoverageTest, RangeWithHolesIsCoveredExactlyOnce) {
    createCollection();
    insertDocuments(1000);
    // Delete most of the range so that the surviving records are sparse and the strided pivots land
    // on RecordIds no record holds. Each slice then has to seek forward to its first live record
    // without skipping the records its predecessor stopped short of.
    std::vector<int> toDelete;
    for (int i = 1; i <= 1000; ++i) {
        if (i % 7 != 0) {
            toDelete.push_back(i);
        }
    }
    deleteDocuments(toDelete);
    for (size_t sliceCount : {2, 3, 7, 16, 64, 128}) {
        assertSlicedScanMatchesSerialScan(sliceCount);
    }
}

TEST_F(RecordStoreSliceCoverageTest, LeadingAndTrailingHolesAreCoveredExactlyOnce) {
    createCollection();
    insertDocuments(200);
    // The slicer reads its bounds off the record store, so deleting at both ends shifts the range
    // rather than leaving pivots outside it. The first pivot must still be at or before the first
    // surviving record, and the open-ended last slice must still reach the last one.
    deleteDocuments({1, 2, 3, 4, 5, 196, 197, 198, 199, 200});
    for (size_t sliceCount : {2, 5, 32}) {
        assertSlicedScanMatchesSerialScan(sliceCount);
    }
}

TEST_F(RecordStoreSliceCoverageTest, NarrowAndDegenerateRangesAreCoveredExactlyOnce) {
    createCollection();
    // Ranges too narrow to stride collapse to a single slice, which still has to cover everything.
    insertDocuments(3);
    for (size_t sliceCount : {1, 2, 8, 64}) {
        assertSlicedScanMatchesSerialScan(sliceCount);
    }
}

TEST_F(RecordStoreSliceCoverageTest, EmptyRecordStoreCoversNothing) {
    createCollection();
    // No pivots means no slices, and nothing to traverse.
    EXPECT_THAT(slicedScan(pivots(8)), IsEmpty());
}

TEST_F(RecordStoreSliceCoverageTest, StringRecordIdsAreCoveredExactlyOnce) {
    createClusteredCollection();
    insertDocuments(100);
    for (size_t sliceCount : {1, 4, 16}) {
        assertSlicedScanMatchesSerialScan(sliceCount);
    }
}

}  // namespace
}  // namespace collection_validation
}  // namespace mongo
