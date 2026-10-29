// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
