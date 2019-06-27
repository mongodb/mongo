/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/executor/thread_pool_task_executor_test_fixture.h"

#include <memory>

#include "mongo/executor/thread_pool_mock.h"

namespace mongo {
namespace executor {

std::unique_ptr<ThreadPoolTaskExecutor> makeThreadPoolTestExecutor(
    std::unique_ptr<NetworkInterfaceMock> net, ThreadPoolMock::Options options) {
    auto netPtr = net.get();
    return std::make_unique<ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPoolMock>(netPtr, 1, std::move(options)), std::move(net));
}

std::shared_ptr<ThreadPoolTaskExecutor> makeSharedThreadPoolTestExecutor(
    std::unique_ptr<NetworkInterfaceMock> net, ThreadPoolMock::Options options) {
    auto netPtr = net.get();
    return std::make_shared<ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPoolMock>(netPtr, 1, std::move(options)), std::move(net));
}

ThreadPoolExecutorTest::ThreadPoolExecutorTest() {}

ThreadPoolExecutorTest::ThreadPoolExecutorTest(ThreadPoolMock::Options options)
    : _options(std::move(options)) {}

ThreadPoolMock::Options ThreadPoolExecutorTest::makeThreadPoolMockOptions() const {
    return _options;
}

std::unique_ptr<TaskExecutor> ThreadPoolExecutorTest::makeTaskExecutor(
    std::unique_ptr<NetworkInterfaceMock> net) {
    auto options = makeThreadPoolMockOptions();
    return makeThreadPoolTestExecutor(std::move(net), std::move(options));
}

}  // namespace executor
}  // namespace mongo
