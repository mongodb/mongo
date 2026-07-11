// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_time_tracker.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(OperationTimeTracker, UnintializedMaxOperationTime) {
    OperationTimeTracker opTimeTracker;
    ASSERT_TRUE(opTimeTracker.getMaxOperationTime() == LogicalTime::kUninitialized);
}

TEST(OperationTimeTracker, UpdateOperationTimeStoresTime) {
    OperationTimeTracker opTimeTracker;

    LogicalTime time(Timestamp(10));
    opTimeTracker.updateOperationTime(time);

    ASSERT_TRUE(opTimeTracker.getMaxOperationTime() == time);
}

TEST(OperationTimeTracker, UpdateOperationTimeKeepsLaterTime) {
    OperationTimeTracker opTimeTracker;

    LogicalTime time(Timestamp(10));
    opTimeTracker.updateOperationTime(time);

    LogicalTime laterTime(Timestamp(15));
    opTimeTracker.updateOperationTime(laterTime);

    ASSERT_TRUE(opTimeTracker.getMaxOperationTime() == laterTime);
}

TEST(OperationTimeTracker, UpdateOperationTimeRejectsLowerTime) {
    OperationTimeTracker opTimeTracker;

    LogicalTime time(Timestamp(10));
    opTimeTracker.updateOperationTime(time);

    LogicalTime lowerTime(Timestamp(5));
    opTimeTracker.updateOperationTime(lowerTime);

    ASSERT_TRUE(opTimeTracker.getMaxOperationTime() == time);
}

}  // unnamed namespace
}  // namespace mongo
