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

#include <boost/none.hpp>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

enum class IndexType { kUnique, kNonUnique, kId };

// Tests findLoc when it hits something.
void testFindLoc_Hit(IndexType type) {
    const auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface();
    } else {
        sorted = harnessHelper->newSortedDataInterface(type == IndexType::kUnique,
                                                       /*partial=*/false);
    }

    auto opCtx = harnessHelper->newOperationContext();

    insertToIndex(opCtx.get(),
                  sorted.get(),
                  {
                      {key1, loc1},
                      {key2, loc1},
                      {key3, loc1},
                  },
                  /* dupsAllowed*/ false);

    auto loc =
        sorted->findLoc(opCtx.get(), makeKeyStringForSeek(sorted.get(), key2).finishAndGetBuffer());
    ASSERT_EQ(loc, loc1);
}

TEST(SortedDataInterface, SeekExact_Hit_Unique) {
    testFindLoc_Hit(IndexType::kUnique);
}

TEST(SortedDataInterface, SeekExact_Hit_Standard) {
    testFindLoc_Hit(IndexType::kNonUnique);
}

TEST(SortedDataInterface, SeekExact_Hit_Id) {
    testFindLoc_Hit(IndexType::kId);
}

// Tests findLoc when it doesn't hit the query.
void testFindLoc_Miss(IndexType type) {
    const auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    std::unique_ptr<SortedDataInterface> sorted;
    if (IndexType::kId == type) {
        sorted = harnessHelper->newIdIndexSortedDataInterface();
    } else {
        sorted = harnessHelper->newSortedDataInterface(type == IndexType::kUnique,
                                                       /*partial=*/false);
    }

    auto opCtx = harnessHelper->newOperationContext();
    insertToIndex(opCtx.get(),
                  sorted.get(),
                  {
                      {key1, loc1},
                      // No key 2
                      {key3, loc1},
                  },
                  /* dupsAllowed*/ false);

    ASSERT_EQ(
        sorted->findLoc(opCtx.get(), makeKeyStringForSeek(sorted.get(), key2).finishAndGetBuffer()),
        boost::none);
}

TEST(SortedDataInterface, SeekExact_Miss_Unique) {
    testFindLoc_Miss(IndexType::kUnique);
}

TEST(SortedDataInterface, SeekExact_Miss_Standard) {
    testFindLoc_Miss(IndexType::kNonUnique);
}

TEST(SortedDataInterface, SeekExact_Miss_Id) {
    testFindLoc_Miss(IndexType::kId);
}

}  // namespace
}  // namespace mongo
