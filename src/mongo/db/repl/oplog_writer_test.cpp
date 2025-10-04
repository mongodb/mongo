/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_writer.h"

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace repl {
namespace {


/**
 * No-op implementation of OplogWriter whose entire
 * job is to throw an exception during the _run() loop.
 */
class FailingOplogWriter : public OplogWriter {
private:
public:
    FailingOplogWriter(executor::TaskExecutor* executor, const Options& options)
        : OplogWriter(executor, nullptr, options) {};
    bool writeOplogBatch(OperationContext* opCtx, const std::vector<BSONObj>& ops) override;

    bool scheduleWriteOplogBatch(OperationContext* opCtx,
                                 const std::vector<OplogEntry>& ops) override;

    void waitForScheduledWrites(OperationContext* opCtx) override;

private:
    void _run() override;
};

void FailingOplogWriter::_run() {
    iasserted(ErrorCodes::UnknownError, "throwing in OplogWriter");
}

bool FailingOplogWriter::writeOplogBatch(OperationContext* opCtx, const std::vector<BSONObj>& ops) {
    return true;
}
bool FailingOplogWriter::scheduleWriteOplogBatch(OperationContext* opCtx,
                                                 const std::vector<OplogEntry>& ops) {
    return true;
};

void FailingOplogWriter::waitForScheduledWrites(OperationContext* opCtx) {};


/**
 * Tests for the parent class behavior for OplogWriter.
 * Uses a default ThreadPoolExecutor.
 */
class OplogWriterTest : public ServiceContextMongoDTest {};


/**
 * OplogWriter internally runs its work on a ThreadPool task.
 * ThreadPool expects tasks to never throw, and if they do,
 * it will hit a noexcept boundary and terminate the process
 * with a stack trace.
 *
 * In rare cases it is possible to throw from OplogWriterImpl;
 * see https://jira.mongodb.org/browse/SERVER-101858 . When this
 * happens it's useful to give the user a human-readable
 * log message alongside the backtrace.
 */
DEATH_TEST_F(OplogWriterTest, ThrowingInRunLoopLogsUsefulError, "OplogWriter threw a DBException") {
    executor::ThreadPoolMock::Options threadPoolMockOptions;

    executor::ThreadPoolExecutorTest executorFixture{threadPoolMockOptions};
    executorFixture.setUp();
    auto& executor = executorFixture.getExecutor();
    executor.startup();

    OplogWriter::Options options(false /* skipWritesToOplogColl */,
                                 false /* skipWritesToChangeColl */);

    FailingOplogWriter oplogWriter(&executor, options);

    auto writerFuture = oplogWriter.startup();  // We should throw here and hit the fassert.

    oplogWriter.shutdown();
    writerFuture.wait();
}

}  // namespace
}  // namespace repl
}  // namespace mongo
