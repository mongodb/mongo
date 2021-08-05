/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/executor/executor_stress_test_fixture.h"
#include "mongo/executor/task_executor_test_common.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"

namespace mongo {
namespace executor {
namespace {

MONGO_INITIALIZER(ThreadPoolExecutorCommonTests)(InitializerContext*) {
    addTestsForExecutor("ThreadPoolExecutorCommon", [](std::unique_ptr<NetworkInterfaceMock> net) {
        return makeSharedThreadPoolTestExecutor(std::move(net));
    });
}

TEST_F(ThreadPoolExecutorMockNetStressTest, StressTest) {
#if defined(__APPLE__) && defined(__aarch64__)
    // TODO: Fix this test under mac os arm64.
#else
    // The idea is to have as much concurrency in the 'executor' as possible. However adding
    // too many threads increases contention in the ThreadPoolExecutorStressTestEngine
    // _mutex. Check the stats printed by the waitAndCleanup() for results. Apparently the
    // executor saturates very fast to some extend the thread count has very limited influence.
    addSimpleSchedulingThreads(100);
    addRandomCancelationThreads(20);
    addScheduleRemoteCommandThreads(100);
#endif  // defined(__APPLE__) && defined(__aarch64__)
}

}  // namespace
}  // namespace executor
}  // namespace mongo
