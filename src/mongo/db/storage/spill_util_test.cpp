// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/spill_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <limits>

namespace mongo {
namespace {

// Arbitrary path – irrelevant while the failpoint is active.
const boost::filesystem::path kDbPath{"/tmp"};

// A helper that activates the `simulateAvailableDiskSpace` failpoint with the given byte count,
// calls ensureSufficientDiskSpaceForSpilling, and returns the resulting Status.
Status checkDiskSpace(int64_t availableBytes, int64_t minRequired) {
    FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << availableBytes)};
    return ensureSufficientDiskSpaceForSpilling(kDbPath, minRequired);
}

TEST(EnsureSufficientDiskSpaceForSpillingTest, SufficientSpace) {
    // available >= minRequired: spilling must be allowed (the check is strict `<`).
    const int64_t minRequired = 500LL * 1024 * 1024;
    ASSERT_OK(checkDiskSpace(minRequired, minRequired));      // exactly at threshold
    ASSERT_OK(checkDiskSpace(minRequired + 1, minRequired));  // just above
}

TEST(EnsureSufficientDiskSpaceForSpillingTest, InsufficientSpace) {
    // available < minRequired: spilling must be rejected with OutOfDiskSpace.
    // The error message must include both byte counts for diagnosability.
    const int64_t minRequired = 500LL * 1024 * 1024;
    auto status = checkDiskSpace(minRequired - 1, minRequired);
    EXPECT_EQ(status.code(), ErrorCodes::OutOfDiskSpace);
    ASSERT_STRING_CONTAINS(status.reason(), std::to_string(minRequired - 1));
    ASSERT_STRING_CONTAINS(status.reason(), std::to_string(minRequired));
}

TEST(EnsureSufficientDiskSpaceForSpillingTest, ZeroMinRequiredAlwaysSucceeds) {
    // When no minimum is required (minRequired == 0) the call must always succeed, even with
    // zero available bytes.
    ASSERT_OK(checkDiskSpace(0, 0));
    ASSERT_OK(checkDiskSpace(1, 0));
}

// TODO(SERVER-121744): Update this test when we update the testing about appropriate fallback
// behavior.
TEST(EnsureSufficientDiskSpaceForSpillingTest, UnknownDiskSpaceAllowsSpilling) {
    // When the OS cannot report disk statistics, getAvailableDiskSpaceBytesInDbPath returns
    // INT64_MAX (optimistic policy: do not block spilling when we cannot measure). Verify that
    // ensureSufficientDiskSpaceForSpilling honours this by treating INT64_MAX as sufficient.
    const int64_t simulatedUnknown = std::numeric_limits<int64_t>::max();
    const int64_t minRequired = 500LL * 1024 * 1024;
    ASSERT_OK(checkDiskSpace(simulatedUnknown, minRequired));
}

}  // namespace
}  // namespace mongo
