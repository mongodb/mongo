// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/disk_space_monitor.h"

#include "mongo/db/operation_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <utility>

namespace mongo {
namespace {

class DiskSpaceMonitorTest : public unittest::Test {

protected:
    DiskSpaceMonitor monitor;
};

class SimpleAction {
public:
    explicit SimpleAction(int& hits) : hits(hits) {}
    int64_t registerSimpleAction(DiskSpaceMonitor& diskMonitor) {
        std::function<int64_t()> getThresholdBytes = []() {
            return 1024;
        };
        std::function<void(OperationContext*, int64_t, int64_t)> act =
            [this](OperationContext* opCtx, int64_t availableBytes, int64_t thresholdBytes) {
                hits += 1;
            };
        return diskMonitor.registerAction(getThresholdBytes, act);
    }

    int& hits;
};

TEST_F(DiskSpaceMonitorTest, Threshold) {
    OperationContext* opCtx = nullptr;
    auto hitsCounter = 0;
    auto action = std::make_unique<SimpleAction>(hitsCounter);
    int64_t actionId = action->registerSimpleAction(monitor);

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 2000)};
        monitor.runAllActions(opCtx);
        EXPECT_EQ(0, hitsCounter);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1024)};
        monitor.runAllActions(opCtx);
        EXPECT_EQ(1, hitsCounter);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1000)};
        monitor.runAllActions(opCtx);
        EXPECT_EQ(2, hitsCounter);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 2000)};
        monitor.runAllActions(opCtx);
        EXPECT_EQ(2, hitsCounter);
    }

    monitor.deregisterAction(actionId);
}

TEST_F(DiskSpaceMonitorTest, TwoActions) {
    OperationContext* opCtx = nullptr;
    auto hitsCounter1 = 0;
    auto hitsCounter2 = 0;
    auto action1 = std::make_unique<SimpleAction>(hitsCounter1);
    auto action2 = std::make_unique<SimpleAction>(hitsCounter2);
    int64_t action1Id = action1->registerSimpleAction(monitor);
    int64_t action2Id = action2->registerSimpleAction(monitor);

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 2000)};

        // Check both actions don't get incremented.
        monitor.runAllActions(opCtx);
        EXPECT_EQ(0, hitsCounter1);
        EXPECT_EQ(0, hitsCounter2);

        monitor.runAction(opCtx, action1Id);
        EXPECT_EQ(0, hitsCounter1);
        EXPECT_EQ(0, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        EXPECT_EQ(0, hitsCounter1);
        EXPECT_EQ(0, hitsCounter2);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1024)};

        // Check both actions get incremented.
        monitor.runAllActions(opCtx);
        EXPECT_EQ(1, hitsCounter1);
        EXPECT_EQ(1, hitsCounter2);

        monitor.runAction(opCtx, action1Id);
        EXPECT_EQ(2, hitsCounter1);
        EXPECT_EQ(1, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        EXPECT_EQ(2, hitsCounter1);
        EXPECT_EQ(2, hitsCounter2);
    }

    // Deregister action1.
    monitor.deregisterAction(action1Id);

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1000)};

        // Check that we increment action2.
        monitor.runAllActions(opCtx);
        EXPECT_EQ(2, hitsCounter1);
        EXPECT_EQ(3, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        EXPECT_EQ(2, hitsCounter1);
        EXPECT_EQ(4, hitsCounter2);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 2000)};

        // Check both actions remain unchanged.
        monitor.runAllActions(opCtx);
        EXPECT_EQ(2, hitsCounter1);
        EXPECT_EQ(4, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        EXPECT_EQ(2, hitsCounter1);
        EXPECT_EQ(4, hitsCounter2);

        monitor.deregisterAction(action2Id);
    }
}

}  // namespace
}  // namespace mongo
