// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/modules.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace executor {

/**
 * Makes a new ThreadPoolTaskExecutor for use in unit tests.
 */
std::shared_ptr<ThreadPoolTaskExecutor> makeThreadPoolTestExecutor(
    std::unique_ptr<NetworkInterfaceMock> net,
    executor::ThreadPoolMock::Options options = executor::ThreadPoolMock::Options());

/**
 * Useful fixture class for tests that use a ThreadPoolTaskExecutor.
 */
class [[MONGO_MOD_OPEN]] ThreadPoolExecutorTest : public TaskExecutorTest {
public:
    /**
     * This default constructor supports the use of this class as a base class for a test fixture.
     */
    ThreadPoolExecutorTest();

    /**
     * This constructor supports the use of this class as a member variable to be used alongside
     * another test fixture. Accepts a ThreadPoolMock::Options that will be used to override the
     * result of makeThreadPoolMockOptions().
     */
    explicit ThreadPoolExecutorTest(ThreadPoolMock::Options options);

private:
    virtual ThreadPoolMock::Options makeThreadPoolMockOptions() const;
    std::shared_ptr<TaskExecutor> makeTaskExecutor(
        std::unique_ptr<NetworkInterfaceMock> net) override;

    // Returned by makeThreadPoolMockOptions().
    const ThreadPoolMock::Options _options = {};
};

}  // namespace executor
}  // namespace mongo
