// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
class OplogWriterTest : public executor::ThreadPoolExecutorTest {};


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
using OplogWriterTestDeathTest = OplogWriterTest;
DEATH_TEST_F(OplogWriterTestDeathTest,
             ThrowingInRunLoopLogsUsefulError,
             "OplogWriter threw a DBException") {
    auto& executor = getExecutor();
    executor.startup();

    OplogWriter::Options options(false /* skipWritesToOplogColl */);

    FailingOplogWriter oplogWriter(&executor, options);

    auto writerFuture = oplogWriter.startup();  // We should throw here and hit the fassert.

    oplogWriter.shutdown();
    writerFuture.wait();
}

}  // namespace
}  // namespace repl
}  // namespace mongo
