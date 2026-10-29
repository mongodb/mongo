// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/status.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class Client;
class OperationContext;

namespace repl {

class TaskRunner;

/**
 * Test fixture for tests that require a TaskRunner and/or
 * ThreadPool.
 */
class TaskRunnerTest : public ServiceContextTest {
public:
    static Status getDetectableErrorStatus();

    ThreadPool& getThreadPool() const;
    TaskRunner& getTaskRunner() const;

    void destroyTaskRunner();

protected:
    void setUp() override;
    void tearDown() override;

private:
    std::unique_ptr<ThreadPool> _threadPool;
    std::unique_ptr<TaskRunner> _taskRunner;
};

}  // namespace repl
}  // namespace mongo
