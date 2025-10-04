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

#pragma once

#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/modules.h"

#include <memory>

namespace MONGO_MOD_PUB mongo {
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
class MONGO_MOD_OPEN ThreadPoolExecutorTest : public TaskExecutorTest {
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
}  // namespace MONGO_MOD_PUB mongo
