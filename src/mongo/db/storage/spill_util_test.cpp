/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
    ASSERT_EQ(status.code(), ErrorCodes::OutOfDiskSpace);
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
