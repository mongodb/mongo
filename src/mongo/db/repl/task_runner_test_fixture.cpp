/**
 *    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/task_runner_test_fixture.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/concurrency/old_thread_pool.h"

namespace mongo {
namespace repl {

using namespace mongo;
using namespace mongo::repl;

namespace {

const int kNumThreads = 3;

AtomicInt32 _nextId;

class TaskRunnerOperationContext : public OperationContextNoop {
public:
    TaskRunnerOperationContext() : _id(_nextId.fetchAndAdd(1)) {}
    int getId() const {
        return _id;
    }

private:
    int _id;
};


}  // namespace

Status TaskRunnerTest::getDetectableErrorStatus() {
    return Status(ErrorCodes::InternalError, "Not mutated");
}

int TaskRunnerTest::getOperationContextId(OperationContext* txn) {
    if (!txn) {
        return -1;
    }
    TaskRunnerOperationContext* taskRunnerTxn = dynamic_cast<TaskRunnerOperationContext*>(txn);
    if (!taskRunnerTxn) {
        return -2;
    }
    return taskRunnerTxn->getId();
}

OperationContext* TaskRunnerTest::createOperationContext() const {
    return new TaskRunnerOperationContext();
}

TaskRunner& TaskRunnerTest::getTaskRunner() const {
    ASSERT(_taskRunner.get());
    return *_taskRunner;
}

OldThreadPool& TaskRunnerTest::getThreadPool() const {
    ASSERT(_threadPool.get());
    return *_threadPool;
}

void TaskRunnerTest::resetTaskRunner(TaskRunner* taskRunner) {
    _taskRunner.reset(taskRunner);
}

void TaskRunnerTest::destroyTaskRunner() {
    _taskRunner.reset();
}

void TaskRunnerTest::setUp() {
    _threadPool.reset(new OldThreadPool(kNumThreads, "TaskRunnerTest-"));
    resetTaskRunner(new TaskRunner(_threadPool.get(),
                                   stdx::bind(&TaskRunnerTest::createOperationContext, this)));
}

void TaskRunnerTest::tearDown() {
    destroyTaskRunner();
    _threadPool.reset();
}

}  // namespace repl
}  // namespace mongo
