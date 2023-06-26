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

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <memory>

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
// Tests findLoc when it hits something.
void testFindLoc_Hit(bool unique) {
    const auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            {key2, loc1},
                                                            {key3, loc1},
                                                        });

    auto loc = sorted->findLoc(opCtx.get(), makeKeyString(sorted.get(), key2));
    ASSERT_EQ(loc, loc1);
}
TEST(SortedDataInterface, SeekExact_Hit_Unique) {
    testFindLoc_Hit(true);
}
TEST(SortedDataInterface, SeekExact_Hit_Standard) {
    testFindLoc_Hit(false);
}

// Tests findLoc when it doesn't hit the query.
void testFindLoc_Miss(bool unique) {
    const auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    auto opCtx = harnessHelper->newOperationContext();
    auto sorted = harnessHelper->newSortedDataInterface(unique,
                                                        /*partial=*/false,
                                                        {
                                                            {key1, loc1},
                                                            // No key2.
                                                            {key3, loc1},
                                                        });

    ASSERT_EQ(sorted->findLoc(opCtx.get(), makeKeyString(sorted.get(), key2)), boost::none);
}
TEST(SortedDataInterface, SeekExact_Miss_Unique) {
    testFindLoc_Miss(true);
}
TEST(SortedDataInterface, SeekExact_Miss_Standard) {
    testFindLoc_Miss(false);
}
}  // namespace
}  // namespace mongo
