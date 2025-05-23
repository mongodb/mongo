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

#include <utility>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class DiskSpaceMonitorTest : public unittest::Test {

protected:
    DiskSpaceMonitor monitor;
};

class SimpleAction : public DiskSpaceMonitor::Action {
public:
    int64_t getThresholdBytes() override {
        return 1024;
    }

    void act(OperationContext* opCtx, int64_t availableBytes) override {
        hits += 1;
    }

    int hits = 0;
};

TEST_F(DiskSpaceMonitorTest, Threshold) {
    OperationContext* opCtx = nullptr;
    auto action = std::make_unique<SimpleAction>();
    auto actionPtr = action.get();
    int64_t actionId = monitor.registerAction(std::move(action));

    monitor.takeAction(opCtx, 2000);
    ASSERT_EQ(0, actionPtr->hits);

    monitor.takeAction(opCtx, 1024);
    ASSERT_EQ(1, actionPtr->hits);

    monitor.takeAction(opCtx, 1000);
    ASSERT_EQ(2, actionPtr->hits);

    monitor.takeAction(opCtx, 2000);
    ASSERT_EQ(2, actionPtr->hits);

    monitor.deregisterAction(actionId);
}

TEST_F(DiskSpaceMonitorTest, TwoActions) {
    OperationContext* opCtx = nullptr;
    auto action1 = std::make_unique<SimpleAction>();
    auto action2 = std::make_unique<SimpleAction>();
    auto action1Ptr = action1.get();
    auto action2Ptr = action2.get();
    int64_t action1Id = monitor.registerAction(std::move(action1));
    int64_t action2Id = monitor.registerAction(std::move(action2));

    // Check both actions don't get incremented.
    monitor.takeAction(opCtx, 2000);
    ASSERT_EQ(0, action1Ptr->hits);
    ASSERT_EQ(0, action2Ptr->hits);

    // Check both actions get incremented.
    monitor.takeAction(opCtx, 1024);
    ASSERT_EQ(1, action1Ptr->hits);
    ASSERT_EQ(1, action2Ptr->hits);

    // Deregister action1.
    monitor.deregisterAction(action1Id);

    // Check that we increment action2, and that action1 remains unchanged.
    monitor.takeAction(opCtx, 1000);
    ASSERT_EQ(1, action1Ptr->hits);
    ASSERT_EQ(2, action2Ptr->hits);

    // Check both actions remain unchanged.
    monitor.takeAction(opCtx, 2000);
    ASSERT_EQ(1, action1Ptr->hits);
    ASSERT_EQ(2, action2Ptr->hits);
    monitor.deregisterAction(action2Id);
}

}  // namespace
}  // namespace mongo
