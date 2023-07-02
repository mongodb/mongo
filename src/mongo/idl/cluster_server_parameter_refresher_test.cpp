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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/cluster_server_parameter_refresher.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
class ClusterServerParameterRefresherTest : public ServiceContextTest {
public:
    void setUp() override {
        _opCtx = getClient()->makeOperationContext();
        _refresher = std::make_unique<ClusterServerParameterRefresher>();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ClusterServerParameterRefresher> _refresher;
};

TEST_F(ClusterServerParameterRefresherTest, testRefresherConcurrency) {
    auto runRefreshTestIteration = [&](StringData blockingFPName, Status expectedStatus) {
        ASSERT_EQ(_refresher->_refreshPromise, nullptr);
        // Set up failpoints and get their initial times entered.
        auto blockFp = globalFailPointRegistry().find(blockingFPName);
        auto waiterCountFp =
            globalFailPointRegistry().find("countPromiseWaitersClusterParameterRefresh");
        auto initBlockFpTE = blockFp->setMode(FailPoint::alwaysOn);
        auto initWaiterFpTE = waiterCountFp->setMode(FailPoint::alwaysOn);

        // Since there should be no active job at this point, we expect thread 1 to create the
        // promise, run _refreshParameters, and block on the failpoint.
        stdx::thread firstRun([&]() {
            Status status = _refresher->refreshParameters(opCtx());
            ASSERT_EQ(status, expectedStatus);
        });

        // Wait for thread 1 to reach the blocking failpoint. Note that each time we enter the
        // blocking failpoint, we increment timesEntered by 2, because we first check for shouldFail
        // and then call pauseWhileSet.
        blockFp->waitForTimesEntered(initBlockFpTE + 2);
        ASSERT(_refresher->_refreshPromise && !_refresher->_refreshPromise->getFuture().isReady());

        // Toggle the countPromiseWaiters failpoint to get the times entered. This count should not
        // have changed from the initial count as we have no futures waiting on the promise yet.
        waiterCountFp->setMode(FailPoint::off);
        auto waiterCountAfterBlock = waiterCountFp->setMode(FailPoint::alwaysOn);
        ASSERT_EQ(waiterCountAfterBlock, initWaiterFpTE);

        // Threads 2 and 3 should both see that there is an active promise and take out a future on
        // it, not entering _refreshParameters themselves.
        stdx::thread secondRun([&]() {
            Status status = _refresher->refreshParameters(opCtx());
            ASSERT_EQ(status, expectedStatus);
        });
        stdx::thread thirdRun([&]() {
            Status status = _refresher->refreshParameters(opCtx());
            ASSERT_EQ(status, expectedStatus);
        });

        // Allow both new threads to hit the future wait before unblocking the first thread
        waiterCountFp->waitForTimesEntered(initWaiterFpTE + 2);
        waiterCountFp->setMode(FailPoint::off);

        // We expect that neither of threads 2 and 3 entered _refreshParameters, so neither should
        // have hit the blocking failpoint; assert its count is the same as before.
        auto afterSleepTE = blockFp->setMode(FailPoint::off);
        ASSERT_EQ(afterSleepTE, initBlockFpTE + 2);

        // The first thread should now finish, setting the job to ready and notifying threads 2 and
        // 3, which should finish.
        firstRun.join();
        ASSERT_EQ(_refresher->_refreshPromise, nullptr);

        secondRun.join();
        thirdRun.join();
    };

    const int major_iters = 3;
    const int minor_iters = 2;
    for (int i = 0; i < major_iters; i++) {
        // Interlace testing of the OK and failure cases to ensure that we are never getting a stale
        // status.
        for (int j = 0; j < minor_iters; j++) {
            runRefreshTestIteration("blockAndSucceedClusterParameterRefresh", Status::OK());
        }
        for (int j = 0; j < minor_iters; j++) {
            // Note that status comparison only cares about error code.
            runRefreshTestIteration("blockAndFailClusterParameterRefresh",
                                    Status(ErrorCodes::FailPointEnabled, "..."));
        }
    }
}
}  // namespace
}  // namespace mongo
