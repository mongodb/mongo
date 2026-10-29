// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <boost/none.hpp>

namespace mongo {
namespace {

enum class IndexType { kUnique, kNonUnique, kId };

// Tests findLoc when it hits something.
void testFindLoc_Hit(OperationContext* opCtx,
                     RecoveryUnit& ru,
                     SortedDataInterfaceHarnessHelper* harnessHelper,
                     IndexType type) {
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface(opCtx);
    } else {
        sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                       type == IndexType::kUnique,
                                                       /*partial=*/false);
    }

    insertToIndex(opCtx,
                  sorted.get(),
                  {
                      {key1, loc1},
                      {key2, loc1},
                      {key3, loc1},
                  },
                  /* dupsAllowed*/ false);

    auto loc =
        sorted->findLoc(opCtx, ru, makeKeyStringForSeek(sorted.get(), key2).finishAndGetBuffer());
    ASSERT_EQ(loc, loc1);
}

TEST_F(SortedDataInterfaceTest, SeekExact_Hit_Unique) {
    testFindLoc_Hit(opCtx(), recoveryUnit(), harnessHelper(), IndexType::kUnique);
}

TEST_F(SortedDataInterfaceTest, SeekExact_Hit_Standard) {
    testFindLoc_Hit(opCtx(), recoveryUnit(), harnessHelper(), IndexType::kNonUnique);
}

TEST_F(SortedDataInterfaceTest, SeekExact_Hit_Id) {
    testFindLoc_Hit(opCtx(), recoveryUnit(), harnessHelper(), IndexType::kId);
}

// Tests findLoc when it doesn't hit the query.
void testFindLoc_Miss(OperationContext* opCtx,
                      RecoveryUnit& ru,
                      SortedDataInterfaceHarnessHelper* harnessHelper,
                      IndexType type) {
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface(opCtx);
    } else {
        sorted = harnessHelper->newSortedDataInterface(opCtx,
                                                       type == IndexType::kUnique,
                                                       /*partial=*/false);
    }

    insertToIndex(opCtx,
                  sorted.get(),
                  {
                      {key1, loc1},
                      // No key 2
                      {key3, loc1},
                  },
                  /* dupsAllowed*/ false);

    ASSERT_EQ(
        sorted->findLoc(opCtx, ru, makeKeyStringForSeek(sorted.get(), key2).finishAndGetBuffer()),
        boost::none);
}

TEST_F(SortedDataInterfaceTest, SeekExact_Miss_Unique) {
    testFindLoc_Miss(opCtx(), recoveryUnit(), harnessHelper(), IndexType::kUnique);
}

TEST_F(SortedDataInterfaceTest, SeekExact_Miss_Standard) {
    testFindLoc_Miss(opCtx(), recoveryUnit(), harnessHelper(), IndexType::kNonUnique);
}

TEST_F(SortedDataInterfaceTest, SeekExact_Miss_Id) {
    testFindLoc_Miss(opCtx(), recoveryUnit(), harnessHelper(), IndexType::kId);
}

}  // namespace
}  // namespace mongo
