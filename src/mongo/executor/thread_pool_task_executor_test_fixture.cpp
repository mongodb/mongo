// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/thread_pool_task_executor_test_fixture.h"

#include "mongo/executor/thread_pool_mock.h"

#include <memory>
#include <utility>

namespace mongo {
namespace executor {

std::shared_ptr<ThreadPoolTaskExecutor> makeThreadPoolTestExecutor(
    std::unique_ptr<NetworkInterfaceMock> net, ThreadPoolMock::Options options) {
    auto netPtr = net.get();
    return ThreadPoolTaskExecutor::create(
        std::make_unique<ThreadPoolMock>(netPtr, 1, std::move(options)), std::move(net));
}

ThreadPoolExecutorTest::ThreadPoolExecutorTest() {}

ThreadPoolExecutorTest::ThreadPoolExecutorTest(ThreadPoolMock::Options options)
    : _options(std::move(options)) {}

ThreadPoolMock::Options ThreadPoolExecutorTest::makeThreadPoolMockOptions() const {
    return _options;
}

std::shared_ptr<TaskExecutor> ThreadPoolExecutorTest::makeTaskExecutor(
    std::unique_ptr<NetworkInterfaceMock> net) {
    auto options = makeThreadPoolMockOptions();
    return makeThreadPoolTestExecutor(std::move(net), std::move(options));
}

}  // namespace executor
}  // namespace mongo
