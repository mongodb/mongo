/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
        ASSERT_EQ(0, hitsCounter);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1024)};
        monitor.runAllActions(opCtx);
        ASSERT_EQ(1, hitsCounter);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1000)};
        monitor.runAllActions(opCtx);
        ASSERT_EQ(2, hitsCounter);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 2000)};
        monitor.runAllActions(opCtx);
        ASSERT_EQ(2, hitsCounter);
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
        ASSERT_EQ(0, hitsCounter1);
        ASSERT_EQ(0, hitsCounter2);

        monitor.runAction(opCtx, action1Id);
        ASSERT_EQ(0, hitsCounter1);
        ASSERT_EQ(0, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        ASSERT_EQ(0, hitsCounter1);
        ASSERT_EQ(0, hitsCounter2);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1024)};

        // Check both actions get incremented.
        monitor.runAllActions(opCtx);
        ASSERT_EQ(1, hitsCounter1);
        ASSERT_EQ(1, hitsCounter2);

        monitor.runAction(opCtx, action1Id);
        ASSERT_EQ(2, hitsCounter1);
        ASSERT_EQ(1, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        ASSERT_EQ(2, hitsCounter1);
        ASSERT_EQ(2, hitsCounter2);
    }

    // Deregister action1.
    monitor.deregisterAction(action1Id);

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 1000)};

        // Check that we increment action2.
        monitor.runAllActions(opCtx);
        ASSERT_EQ(2, hitsCounter1);
        ASSERT_EQ(3, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        ASSERT_EQ(2, hitsCounter1);
        ASSERT_EQ(4, hitsCounter2);
    }

    {
        FailPointEnableBlock fp{"simulateAvailableDiskSpace", BSON("bytes" << 2000)};

        // Check both actions remain unchanged.
        monitor.runAllActions(opCtx);
        ASSERT_EQ(2, hitsCounter1);
        ASSERT_EQ(4, hitsCounter2);

        monitor.runAction(opCtx, action2Id);
        ASSERT_EQ(2, hitsCounter1);
        ASSERT_EQ(4, hitsCounter2);

        monitor.deregisterAction(action2Id);
    }
}

}  // namespace
}  // namespace mongo
