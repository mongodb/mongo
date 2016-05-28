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

#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/applier.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/barrier.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

using Operations = Applier::Operations;

class ApplierTest : public ReplicationExecutorTest {
public:
    Applier* getApplier() const;

protected:
    void setUp() override;
    void tearDown() override;

    /**
     * Test function to check behavior when we fail to apply one of the operations.
     */
    void _testApplyOperationFailed(size_t opIndex, stdx::function<Status()> fail);

    std::unique_ptr<Applier> _applier;
    std::unique_ptr<unittest::Barrier> _barrier;
};

void ApplierTest::setUp() {
    ReplicationExecutorTest::setUp();
    launchExecutorThread();
    auto apply = [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); };
    _applier.reset(new Applier(&getReplExecutor(),
                               {OplogEntry(BSON("ts" << Timestamp(Seconds(123), 0)))},
                               apply,
                               [this](const StatusWith<Timestamp>&, const Operations&) {
                                   if (_barrier.get()) {
                                       _barrier->countDownAndWait();
                                   }
                               }));
}

void ApplierTest::tearDown() {
    ReplicationExecutorTest::tearDown();
    _applier.reset();
    _barrier.reset();
}

Applier* ApplierTest::getApplier() const {
    return _applier.get();
}

TEST_F(ApplierTest, InvalidConstruction) {
    const Operations operations{OplogEntry(BSON("ts" << Timestamp(Seconds(123), 0)))};
    auto apply = [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); };
    auto callback = [](const StatusWith<Timestamp>& status, const Operations& operations) {};

    // Null executor.
    ASSERT_THROWS_CODE(
        Applier(nullptr, operations, apply, callback), UserException, ErrorCodes::BadValue);

    // Empty list of operations.
    ASSERT_THROWS_CODE(
        Applier(&getReplExecutor(), {}, apply, callback), UserException, ErrorCodes::BadValue);

    // Last operation missing timestamp field.
    ASSERT_THROWS_CODE(Applier(&getReplExecutor(), {OplogEntry(BSONObj())}, apply, callback),
                       UserException,
                       ErrorCodes::FailedToParse);

    // "ts" field in last operation not a timestamp.
    ASSERT_THROWS_CODE(Applier(&getReplExecutor(), {OplogEntry(BSON("ts" << 99))}, apply, callback),
                       UserException,
                       ErrorCodes::TypeMismatch);

    // Invalid apply operation function.
    ASSERT_THROWS_CODE(
        Applier(&getReplExecutor(), operations, Applier::ApplyOperationFn(), callback),
        UserException,
        ErrorCodes::BadValue);

    // Invalid callback function.
    ASSERT_THROWS_CODE(Applier(&getReplExecutor(), operations, apply, Applier::CallbackFn()),
                       UserException,
                       ErrorCodes::BadValue);
}

TEST_F(ApplierTest, GetDiagnosticString) {
    ASSERT_FALSE(getApplier()->getDiagnosticString().empty());
}

TEST_F(ApplierTest, IsActiveAfterStart) {
    // Use a barrier to ensure that the callback blocks while
    // we check isActive().
    _barrier.reset(new unittest::Barrier(2U));
    ASSERT_FALSE(getApplier()->isActive());
    ASSERT_OK(getApplier()->start());
    ASSERT_TRUE(getApplier()->isActive());
    _barrier->countDownAndWait();
}

TEST_F(ApplierTest, StartWhenActive) {
    // Use a barrier to ensure that the callback blocks while
    // we check isActive().
    _barrier.reset(new unittest::Barrier(2U));
    ASSERT_OK(getApplier()->start());
    ASSERT_TRUE(getApplier()->isActive());
    ASSERT_NOT_OK(getApplier()->start());
    ASSERT_TRUE(getApplier()->isActive());
    _barrier->countDownAndWait();
}

TEST_F(ApplierTest, CancelWithoutStart) {
    ASSERT_FALSE(getApplier()->isActive());
    getApplier()->cancel();
    ASSERT_FALSE(getApplier()->isActive());
}

TEST_F(ApplierTest, WaitWithoutStart) {
    ASSERT_FALSE(getApplier()->isActive());
    getApplier()->wait();
    ASSERT_FALSE(getApplier()->isActive());
}

TEST_F(ApplierTest, ShutdownBeforeStart) {
    getReplExecutor().shutdown();
    ASSERT_NOT_OK(getApplier()->start());
    ASSERT_FALSE(getApplier()->isActive());
}

TEST_F(ApplierTest, CancelBeforeStartingDBWork) {
    // Schedule a blocking DB work item before the applier to allow us to cancel the applier
    // work item before the executor runs it.
    unittest::Barrier barrier(2U);
    using CallbackData = ReplicationExecutor::CallbackArgs;
    getReplExecutor().scheduleDBWork([&](const CallbackData& cbd) {
        barrier.countDownAndWait();  // generation 0
    });
    const OplogEntry operation(BSON("ts" << Timestamp(Seconds(123), 0)));
    stdx::mutex mutex;
    StatusWith<Timestamp> result = getDetectableErrorStatus();
    Applier::Operations operations;
    _applier.reset(
        new Applier(&getReplExecutor(),
                    {operation},
                    [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); },
                    [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
                        stdx::lock_guard<stdx::mutex> lock(mutex);
                        result = theResult;
                        operations = theOperations;
                    }));

    getApplier()->start();
    getApplier()->cancel();
    ASSERT_TRUE(getApplier()->isActive());

    barrier.countDownAndWait();  // generation 0

    getApplier()->wait();
    ASSERT_FALSE(getApplier()->isActive());

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, result.getStatus().code());
    ASSERT_EQUALS(1U, operations.size());
    ASSERT_EQUALS(operation, operations.front());
}

TEST_F(ApplierTest, DestroyBeforeStartingDBWork) {
    // Schedule a blocking DB work item before the applier to allow us to destroy the applier
    // before the executor runs the work item.
    unittest::Barrier barrier(2U);
    using CallbackData = ReplicationExecutor::CallbackArgs;
    getReplExecutor().scheduleDBWork([&](const CallbackData& cbd) {
        barrier.countDownAndWait();  // generation 0
        // Give the main thread a head start in invoking the applier destructor.
        sleepmillis(1);
    });
    const OplogEntry operation(BSON("ts" << Timestamp(Seconds(123), 0)));
    stdx::mutex mutex;
    StatusWith<Timestamp> result = getDetectableErrorStatus();
    Applier::Operations operations;
    _applier.reset(
        new Applier(&getReplExecutor(),
                    {operation},
                    [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); },
                    [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
                        stdx::lock_guard<stdx::mutex> lock(mutex);
                        result = theResult;
                        operations = theOperations;
                    }));

    getApplier()->start();
    ASSERT_TRUE(getApplier()->isActive());

    barrier.countDownAndWait();  // generation 0

    // It is possible the executor may have invoked the callback before we
    // destroy the applier. Therefore both OK and CallbackCanceled are acceptable
    // statuses.
    _applier.reset();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    if (result.isOK()) {
        ASSERT_TRUE(operations.empty());
    } else {
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, result.getStatus().code());
        ASSERT_EQUALS(1U, operations.size());
        ASSERT_EQUALS(operation, operations.front());
    }
}

TEST_F(ApplierTest, WaitForCompletion) {
    const Timestamp timestamp(Seconds(123), 0);
    stdx::mutex mutex;
    StatusWith<Timestamp> result = getDetectableErrorStatus();
    Applier::Operations operations;
    _applier.reset(
        new Applier(&getReplExecutor(),
                    {OplogEntry(BSON("ts" << timestamp))},
                    [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); },
                    [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
                        stdx::lock_guard<stdx::mutex> lock(mutex);
                        result = theResult;
                        operations = theOperations;
                    }));

    getApplier()->start();
    getApplier()->wait();
    ASSERT_FALSE(getApplier()->isActive());

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(timestamp, result.getValue());
    ASSERT_TRUE(operations.empty());
}

TEST_F(ApplierTest, DestroyShouldBlockUntilInactive) {
    const Timestamp timestamp(Seconds(123), 0);
    unittest::Barrier barrier(2U);
    stdx::mutex mutex;
    StatusWith<Timestamp> result = getDetectableErrorStatus();
    Applier::Operations operations;
    _applier.reset(
        new Applier(&getReplExecutor(),
                    {OplogEntry(BSON("ts" << timestamp))},
                    [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); },
                    [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
                        stdx::lock_guard<stdx::mutex> lock(mutex);
                        result = theResult;
                        operations = theOperations;
                        barrier.countDownAndWait();
                    }));

    getApplier()->start();
    barrier.countDownAndWait();
    _applier.reset();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(timestamp, result.getValue());
    ASSERT_TRUE(operations.empty());
}

TEST_F(ApplierTest, ApplyOperationSuccessful) {
    // Bogus operations codes.
    Applier::Operations operationsToApply{
        OplogEntry(BSON("op"
                        << "a"
                        << "ts"
                        << Timestamp(Seconds(123), 0))),
        OplogEntry(BSON("op"
                        << "b"
                        << "ts"
                        << Timestamp(Seconds(456), 0))),
        OplogEntry(BSON("op"
                        << "c"
                        << "ts"
                        << Timestamp(Seconds(789), 0))),
    };
    stdx::mutex mutex;
    StatusWith<Timestamp> result = getDetectableErrorStatus();
    bool areWritesReplicationOnOperationContext = true;
    bool isLockBatchWriter = false;
    Applier::Operations operationsApplied;
    Applier::Operations operationsOnCompletion;
    auto apply = [&](OperationContext* txn, const OplogEntry& operation) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        areWritesReplicationOnOperationContext = txn->writesAreReplicated();
        isLockBatchWriter = txn->lockState()->isBatchWriter();
        operationsApplied.push_back(operation);
        return Status::OK();
    };
    auto callback = [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        result = theResult;
        operationsOnCompletion = theOperations;
    };

    _applier.reset(new Applier(&getReplExecutor(), operationsToApply, apply, callback));
    _applier->start();
    _applier->wait();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_FALSE(areWritesReplicationOnOperationContext);
    ASSERT_TRUE(isLockBatchWriter);
    ASSERT_EQUALS(operationsToApply.size(), operationsApplied.size());
    ASSERT_EQUALS(operationsToApply[0], operationsApplied[0]);
    ASSERT_EQUALS(operationsToApply[1], operationsApplied[1]);
    ASSERT_EQUALS(operationsToApply[2], operationsApplied[2]);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(operationsToApply[2].raw["ts"].timestamp(), result.getValue());
    ASSERT_TRUE(operationsOnCompletion.empty());
}

void ApplierTest::_testApplyOperationFailed(size_t opIndex, stdx::function<Status()> fail) {
    // Bogus operations codes.
    Applier::Operations operationsToApply{
        OplogEntry(BSON("op"
                        << "a"
                        << "ts"
                        << Timestamp(Seconds(123), 0))),
        OplogEntry(BSON("op"
                        << "b"
                        << "ts"
                        << Timestamp(Seconds(456), 0))),
        OplogEntry(BSON("op"
                        << "c"
                        << "ts"
                        << Timestamp(Seconds(789), 0))),
    };
    stdx::mutex mutex;
    StatusWith<Timestamp> result = getDetectableErrorStatus();
    Applier::Operations operationsApplied;
    Applier::Operations operationsOnCompletion;
    auto apply = [&](OperationContext* txn, const OplogEntry& operation) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (operationsApplied.size() == opIndex) {
            return fail();
        }
        operationsApplied.push_back(operation);
        return Status::OK();
    };
    auto callback = [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        result = theResult;
        operationsOnCompletion = theOperations;
    };

    _applier.reset(new Applier(&getReplExecutor(), operationsToApply, apply, callback));
    _applier->start();
    _applier->wait();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_EQUALS(opIndex, operationsApplied.size());
    size_t i = 0;
    for (const auto& operation : operationsApplied) {
        ASSERT_EQUALS(operationsToApply[i], operation);
        i++;
    }
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result.getStatus().code());
    ASSERT_EQUALS(operationsToApply.size() - opIndex, operationsOnCompletion.size());
    ASSERT_EQUALS(opIndex, i);
    for (const auto& operation : operationsOnCompletion) {
        ASSERT_EQUALS(operationsToApply[i], operation);
        i++;
    }
}

TEST_F(ApplierTest, ApplyOperationFailedOnFirstOperation) {
    _testApplyOperationFailed(0U, []() { return Status(ErrorCodes::OperationFailed, ""); });
}

TEST_F(ApplierTest, ApplyOperationThrowsExceptionOnFirstOperation) {
    _testApplyOperationFailed(0U, []() {
        uasserted(ErrorCodes::OperationFailed, "");
        MONGO_UNREACHABLE;
        return Status(ErrorCodes::InternalError, "unreachable");
    });
}

TEST_F(ApplierTest, ApplyOperationFailedOnSecondOperation) {
    _testApplyOperationFailed(1U, []() { return Status(ErrorCodes::OperationFailed, ""); });
}

TEST_F(ApplierTest, ApplyOperationThrowsExceptionOnSecondOperation) {
    _testApplyOperationFailed(1U, []() {
        uasserted(ErrorCodes::OperationFailed, "");
        MONGO_UNREACHABLE;
        return Status(ErrorCodes::InternalError, "unreachable");
    });
}

TEST_F(ApplierTest, ApplyOperationFailedOnLastOperation) {
    _testApplyOperationFailed(2U, []() { return Status(ErrorCodes::OperationFailed, ""); });
}

TEST_F(ApplierTest, ApplyOperationThrowsExceptionOnLastOperation) {
    _testApplyOperationFailed(2U, []() {
        uasserted(ErrorCodes::OperationFailed, "");
        MONGO_UNREACHABLE;
        return Status(ErrorCodes::InternalError, "unreachable");
    });
}

class ApplyUntilAndPauseTest : public ApplierTest {};

TEST_F(ApplyUntilAndPauseTest, EmptyOperations) {
    auto result = applyUntilAndPause(
        &getReplExecutor(),
        {},
        [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); },
        Timestamp(Seconds(123), 0),
        [] {},
        [](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {});
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST_F(ApplyUntilAndPauseTest, NoOperationsInRange) {
    auto result = applyUntilAndPause(
        &getReplExecutor(),
        {
            OplogEntry(BSON("ts" << Timestamp(Seconds(456), 0))),
            OplogEntry(BSON("ts" << Timestamp(Seconds(789), 0))),
        },
        [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); },
        Timestamp(Seconds(123), 0),
        [] {},
        [](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {});
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST_F(ApplyUntilAndPauseTest, OperationMissingTimestampField) {
    auto result = applyUntilAndPause(
        &getReplExecutor(),
        {OplogEntry(BSONObj())},
        [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); },
        Timestamp(Seconds(123), 0),
        [] {},
        [](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {});
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST_F(ApplyUntilAndPauseTest, ApplyUntilAndPauseSingleOperation) {
    Timestamp ts(Seconds(123), 0);
    const Operations operationsToApply{OplogEntry(BSON("ts" << ts))};
    stdx::mutex mutex;
    StatusWith<Timestamp> completionResult = getDetectableErrorStatus();
    bool pauseCalled = false;
    Applier::Operations operationsOnCompletion;
    auto apply = [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); };
    auto pause = [&] {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        pauseCalled = true;
    };
    auto callback = [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        completionResult = theResult;
        operationsOnCompletion = theOperations;
    };

    auto result =
        applyUntilAndPause(&getReplExecutor(), operationsToApply, apply, ts, pause, callback);
    ASSERT_OK(result.getStatus());
    _applier = std::move(result.getValue().first);
    ASSERT_TRUE(_applier);

    const Applier::Operations& operationsDiscarded = result.getValue().second;
    ASSERT_TRUE(operationsDiscarded.empty());

    _applier->start();
    _applier->wait();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_TRUE(pauseCalled);
    ASSERT_OK(completionResult.getStatus());
    ASSERT_EQUALS(ts, completionResult.getValue());
    ASSERT_TRUE(operationsOnCompletion.empty());
}

TEST_F(ApplyUntilAndPauseTest, ApplyUntilAndPauseSingleOperationTimestampNotInOperations) {
    Timestamp ts(Seconds(123), 0);
    const Operations operationsToApply{OplogEntry(BSON("ts" << ts))};
    stdx::mutex mutex;
    StatusWith<Timestamp> completionResult = getDetectableErrorStatus();
    bool pauseCalled = false;
    Applier::Operations operationsOnCompletion;
    auto apply = [](OperationContext* txn, const OplogEntry& operation) { return Status::OK(); };
    auto pause = [&] {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        pauseCalled = true;
    };
    auto callback = [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        completionResult = theResult;
        operationsOnCompletion = theOperations;
    };

    Timestamp ts2(Seconds(456), 0);
    auto result =
        applyUntilAndPause(&getReplExecutor(), operationsToApply, apply, ts2, pause, callback);
    ASSERT_OK(result.getStatus());
    _applier = std::move(result.getValue().first);
    ASSERT_TRUE(_applier);

    const Applier::Operations& operationsDiscarded = result.getValue().second;
    ASSERT_TRUE(operationsDiscarded.empty());

    _applier->start();
    _applier->wait();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_FALSE(pauseCalled);
    ASSERT_OK(completionResult.getStatus());
    ASSERT_EQUALS(ts, completionResult.getValue());
    ASSERT_TRUE(operationsOnCompletion.empty());
}

TEST_F(ApplyUntilAndPauseTest, ApplyUntilAndPauseSingleOperationAppliedFailed) {
    Timestamp ts(Seconds(123), 0);
    const Operations operationsToApply{OplogEntry(BSON("ts" << ts))};
    stdx::mutex mutex;
    StatusWith<Timestamp> completionResult = getDetectableErrorStatus();
    bool pauseCalled = false;
    Applier::Operations operationsOnCompletion;
    auto apply = [](OperationContext* txn, const OplogEntry& operation) {
        return Status(ErrorCodes::OperationFailed, "");
    };
    auto pause = [&] {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        pauseCalled = true;
    };
    auto callback = [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        completionResult = theResult;
        operationsOnCompletion = theOperations;
    };

    auto result =
        applyUntilAndPause(&getReplExecutor(), operationsToApply, apply, ts, pause, callback);
    ASSERT_OK(result.getStatus());
    _applier = std::move(result.getValue().first);
    ASSERT_TRUE(_applier);

    const Applier::Operations& operationsDiscarded = result.getValue().second;
    ASSERT_TRUE(operationsDiscarded.empty());

    _applier->start();
    _applier->wait();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_FALSE(pauseCalled);
    ASSERT_NOT_OK(completionResult.getStatus());
    ASSERT_FALSE(operationsOnCompletion.empty());
}

void _testApplyUntilAndPauseDiscardOperations(ReplicationExecutor* executor,
                                              const Timestamp& ts,
                                              bool expectedPauseCalled) {
    Applier::Operations operationsToApply{
        OplogEntry(BSON("op"
                        << "a"
                        << "ts"
                        << Timestamp(Seconds(123), 0))),
        OplogEntry(BSON("op"
                        << "b"
                        << "ts"
                        << Timestamp(Seconds(456), 0))),
        OplogEntry(BSON("op"
                        << "c"
                        << "ts"
                        << Timestamp(Seconds(789), 0))),
    };
    stdx::mutex mutex;
    StatusWith<Timestamp> completionResult = ApplyUntilAndPauseTest::getDetectableErrorStatus();
    bool pauseCalled = false;
    Applier::Operations operationsApplied;
    Applier::Operations operationsOnCompletion;
    auto apply = [&](OperationContext* txn, const OplogEntry& operation) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationsApplied.push_back(operation);
        return Status::OK();
    };
    auto pause = [&] {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        pauseCalled = true;
    };
    auto callback = [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        completionResult = theResult;
        operationsOnCompletion = theOperations;
    };

    auto result = applyUntilAndPause(executor, operationsToApply, apply, ts, pause, callback);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().first);
    Applier& applier = *result.getValue().first;

    const Applier::Operations& operationsDiscarded = result.getValue().second;
    ASSERT_EQUALS(1U, operationsDiscarded.size());
    ASSERT_EQUALS(operationsToApply[2], operationsDiscarded[0]);

    applier.start();
    applier.wait();

    stdx::lock_guard<stdx::mutex> lock(mutex);
    ASSERT_EQUALS(2U, operationsApplied.size());
    ASSERT_EQUALS(operationsToApply[0], operationsApplied[0]);
    ASSERT_EQUALS(operationsToApply[1], operationsApplied[1]);
    ASSERT_EQUALS(expectedPauseCalled, pauseCalled);
    ASSERT_OK(completionResult.getStatus());
    ASSERT_TRUE(operationsOnCompletion.empty());
}

TEST_F(ApplyUntilAndPauseTest, ApplyUntilAndPauseDiscardOperationsTimestampInOperations) {
    _testApplyUntilAndPauseDiscardOperations(&getReplExecutor(), Timestamp(Seconds(456), 0), true);
}

TEST_F(ApplyUntilAndPauseTest, ApplyUntilAndPauseDiscardOperationsTimestampNotInOperations) {
    _testApplyUntilAndPauseDiscardOperations(&getReplExecutor(), Timestamp(Seconds(500), 0), false);
}

}  // namespace
