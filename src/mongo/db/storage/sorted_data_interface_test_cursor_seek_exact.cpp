/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
