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

#include "mongo/db/repl/task_runner_test_fixture.h"

#include <functional>

#include "mongo/db/repl/task_runner.h"
#include "mongo/stdx/memory.h"

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
    ThreadPool::Options options;
    options.poolName = "TaskRunnerTest";
    options.onCreateThread = [](const std::string& name) { Client::initThread(name); };
    _threadPool = stdx::make_unique<ThreadPool>(options);
    _threadPool->startup();

    _taskRunner = stdx::make_unique<TaskRunner>(_threadPool.get());
}

void TaskRunnerTest::tearDown() {
    destroyTaskRunner();
    _threadPool.reset();
}

}  // namespace repl
}  // namespace mongo
