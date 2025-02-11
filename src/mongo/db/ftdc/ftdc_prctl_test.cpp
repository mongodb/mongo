/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "mongo/unittest/unittest.h"

namespace mongo {

#ifdef __linux__
TEST(FTDCPrctlTest, InvalidTHPArgs) {
    ASSERT_EQ(prctl(PR_GET_THP_DISABLE, 1, 1, 8, 0), -1);
    ASSERT_EQ(prctl(PR_SET_THP_DISABLE, 0, 2, 5, 17), -1);
}

TEST(FTDCPrctlTest, ValidTHPArgs) {
    int thpDisabledValue = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);

    ON_BLOCK_EXIT([&]() { prctl(PR_SET_THP_DISABLE, thpDisabledValue, 0, 0, 0); });

    ASSERT_GREATER_THAN_OR_EQUALS(thpDisabledValue, 0);
    ASSERT_EQ(prctl(PR_SET_THP_DISABLE, 0, 0, 0, 0), 0);
    ASSERT_EQ(prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0), 0);
}

TEST(FTDCPrctlTest, ToggleTHPDisable) {
    int thpDisabledValue = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);

    ON_BLOCK_EXIT([&]() { prctl(PR_SET_THP_DISABLE, thpDisabledValue, 0, 0, 0); });

    prctl(PR_SET_THP_DISABLE, !thpDisabledValue, 0, 0, 0);
    ASSERT_EQ(prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0), !thpDisabledValue);
}
#endif  // __linux__
}  // namespace mongo
