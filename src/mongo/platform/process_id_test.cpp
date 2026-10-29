// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/process_id.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ProcessId, Comparison) {
    const ProcessId p1 = ProcessId::fromNative(NativeProcessId(1));
    const ProcessId p2 = ProcessId::fromNative(NativeProcessId(2));

    ASSERT_FALSE(p1 == p2);
    ASSERT_TRUE(p1 == p1);

    ASSERT_TRUE(p1 != p2);
    ASSERT_FALSE(p1 != p1);

    ASSERT_TRUE(p1 < p2);
    ASSERT_FALSE(p1 < p1);
    ASSERT_FALSE(p2 < p1);

    ASSERT_TRUE(p1 <= p2);
    ASSERT_TRUE(p1 <= p1);
    ASSERT_FALSE(p2 <= p1);

    ASSERT_TRUE(p2 > p1);
    ASSERT_FALSE(p2 > p2);
    ASSERT_FALSE(p1 > p2);

    ASSERT_TRUE(p2 >= p1);
    ASSERT_TRUE(p2 >= p2);
    ASSERT_FALSE(p1 >= p2);
}

TEST(ProcessId, GetCurrentEqualsSelf) {
    ASSERT_EQUALS(ProcessId::getCurrent(), ProcessId::getCurrent());
}

}  // namespace
}  // namespace mongo
