// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/strong_weak_finish_line.h"

#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

TEST(StrongWeakFinishLineTest, WeakArrivalFollowedByStrong) {
    StrongWeakFinishLine finishLine(100);

    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT(finishLine.arriveStrongly());
    ASSERT(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveStrongly());
    ASSERT_FALSE(finishLine.arriveWeakly());
}

TEST(StrongWeakFinishLineTest, AllWeakArrival) {
    StrongWeakFinishLine finishLine(3);

    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT(finishLine.arriveWeakly());
    ASSERT(finishLine.isReady());
}

TEST(StrongWeakFinishLineTest, LastWeakArrivalAfterStrongReturnsFalse) {
    StrongWeakFinishLine finishLine(3);

    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT(finishLine.arriveStrongly());
    ASSERT(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
}

}  // namespace
}  // namespace mongo
