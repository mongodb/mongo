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

#include "mongo/db/service_context_test_fixture.h"
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
    int64_t getThresholdBytes() noexcept override {
        return 1024;
    }

    void act(OperationContext* opCtx, int64_t availableBytes) noexcept override {
        hits += 1;
    }

    int hits = 0;
};

TEST_F(DiskSpaceMonitorTest, Threshold) {
    OperationContext* opCtx = nullptr;
    auto action = std::make_unique<SimpleAction>();
    auto actionPtr = action.get();
    monitor.registerAction(std::move(action));

    monitor.takeAction(opCtx, 2000);
    ASSERT_EQ(0, actionPtr->hits);

    monitor.takeAction(opCtx, 1024);
    ASSERT_EQ(1, actionPtr->hits);

    monitor.takeAction(opCtx, 1000);
    ASSERT_EQ(2, actionPtr->hits);

    monitor.takeAction(opCtx, 2000);
    ASSERT_EQ(2, actionPtr->hits);
}
}  // namespace
}  // namespace mongo
