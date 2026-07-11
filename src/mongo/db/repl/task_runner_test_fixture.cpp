// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/task_runner_test_fixture.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <memory>
#include <string>

namespace mongo {
namespace repl {

using namespace mongo;
using namespace mongo::repl;

Status TaskRunnerTest::getDetectableErrorStatus() {
    return Status(ErrorCodes::InternalError, "Not mutated");
}

TaskRunner& TaskRunnerTest::getTaskRunner() const {
    ASSERT(_taskRunner.get());
    return *_taskRunner;
}

ThreadPool& TaskRunnerTest::getThreadPool() const {
    ASSERT(_threadPool.get());
    return *_threadPool;
}

void TaskRunnerTest::destroyTaskRunner() {
    _taskRunner.reset();
}

void TaskRunnerTest::setUp() {
    _threadPool = ThreadPool::make({
        .poolName = "TaskRunnerTest",
        .onCreateThread =
            [](const std::string& name) {
                Client::initThread(name, getGlobalServiceContext()->getService());
            },
    });
    _threadPool->startup();

    _taskRunner = std::make_unique<TaskRunner>(_threadPool.get());
}

void TaskRunnerTest::tearDown() {
    destroyTaskRunner();
    _threadPool.reset();
}

}  // namespace repl
}  // namespace mongo
