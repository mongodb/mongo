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
        /**
         * Creates an initial error status suitable for checking if
         * applier has modified the 'status' field in test fixture.
         */
        static Status getDetectableErrorStatus();

        void setUp() override;
        void tearDown() override;
        Applier* getApplier() const;

    protected:
        /**
         * Test function to check behavior when we fail to apply one of the operations.
         */
        void _testApplyOperationFailed(size_t opIndex, stdx::function<Status ()> fail);

        std::unique_ptr<Applier> _applier;
        std::unique_ptr<unittest::Barrier> _barrier;
    };

    Status ApplierTest::getDetectableErrorStatus() {
        return Status(ErrorCodes::InternalError, "Not mutated");
    }

    void ApplierTest::setUp() {
        ReplicationExecutorTest::setUp();
        launchExecutorThread();
        auto apply = [](OperationContext* txn, const BSONObj& operation) { return Status::OK(); };
        _applier.reset(new Applier(&getExecutor(),
                                   {BSON("ts" << Timestamp(Seconds(123), 0))},
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
        const Operations operations{BSON("ts" << Timestamp(Seconds(123), 0))};
        auto apply = [](OperationContext* txn, const BSONObj& operation) { return Status::OK(); };
        auto callback = [](const StatusWith<Timestamp>& status, const Operations& operations) { };

        // Null executor.
        ASSERT_THROWS(Applier(nullptr, operations, apply, callback), UserException);

        // Empty list of operations.
        ASSERT_THROWS(Applier(&getExecutor(), {}, apply, callback), UserException);

        // Last operation missing timestamp field.
        ASSERT_THROWS(Applier(&getExecutor(), {BSONObj()}, apply, callback), UserException);

        // "ts" field in last operation not a timestamp.
        ASSERT_THROWS(Applier(&getExecutor(), {BSON("ts" << 99)}, apply, callback), UserException);

        // Invalid apply operation function.
        ASSERT_THROWS(Applier(&getExecutor(), operations, Applier::ApplyOperationFn(), callback),
                      UserException);

        // Invalid callback function.
        ASSERT_THROWS(Applier(&getExecutor(), operations, apply, Applier::CallbackFn()),
                      UserException);
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
        getExecutor().shutdown();
        ASSERT_NOT_OK(getApplier()->start());
        ASSERT_FALSE(getApplier()->isActive());
    }

    TEST_F(ApplierTest, CancelBeforeStartingDBWork) {
        // Schedule a blocking DB work item before the applier to allow us to cancel the applier
        // work item before the executor runs it.
        unittest::Barrier barrier(2U);
        using CallbackData = ReplicationExecutor::CallbackData;
        getExecutor().scheduleDBWork([&](const CallbackData& cbd) {
            barrier.countDownAndWait(); // generation 0
        });
        const BSONObj operation = BSON("ts" << Timestamp(Seconds(123), 0));
        boost::mutex mutex;
        StatusWith<Timestamp> result = getDetectableErrorStatus();
        Applier::Operations operations;
        _applier.reset(new Applier(
                &getExecutor(),
                {operation},
                [](OperationContext* txn, const BSONObj& operation) { return Status::OK(); },
                [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
            boost::lock_guard<boost::mutex> lock(mutex);
            result = theResult;
            operations = theOperations;
        }));

        getApplier()->start();
        getApplier()->cancel();
        ASSERT_TRUE(getApplier()->isActive());

        barrier.countDownAndWait(); // generation 0

        getApplier()->wait();
        ASSERT_FALSE(getApplier()->isActive());

        boost::lock_guard<boost::mutex> lock(mutex);
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, result.getStatus().code());
        ASSERT_EQUALS(1U, operations.size());
        ASSERT_EQUALS(operation, operations.front());
    }

    TEST_F(ApplierTest, DestroyBeforeStartingDBWork) {
        // Schedule a blocking DB work item before the applier to allow us to destroy the applier
        // before the executor runs the work item.
        unittest::Barrier barrier(2U);
        using CallbackData = ReplicationExecutor::CallbackData;
        getExecutor().scheduleDBWork([&](const CallbackData& cbd) {
            barrier.countDownAndWait(); // generation 0
            // Give the main thread a head start in invoking the applier destructor.
            sleepmillis(1);
        });
        const BSONObj operation = BSON("ts" << Timestamp(Seconds(123), 0));
        boost::mutex mutex;
        StatusWith<Timestamp> result = getDetectableErrorStatus();
        Applier::Operations operations;
        _applier.reset(new Applier(
                &getExecutor(),
                {operation},
                [](OperationContext* txn, const BSONObj& operation) { return Status::OK(); },
                [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
            boost::lock_guard<boost::mutex> lock(mutex);
            result = theResult;
            operations = theOperations;
        }));

        getApplier()->start();
        ASSERT_TRUE(getApplier()->isActive());

        barrier.countDownAndWait(); // generation 0

        // It is possible the executor may have invoked the callback before we
        // destroy the applier. Therefore both OK and CallbackCanceled are acceptable
        // statuses.
        _applier.reset();

        boost::lock_guard<boost::mutex> lock(mutex);
        if (result.isOK()) {
            ASSERT_TRUE(operations.empty());
        }
        else {
            ASSERT_EQUALS(ErrorCodes::CallbackCanceled, result.getStatus().code());
            ASSERT_EQUALS(1U, operations.size());
            ASSERT_EQUALS(operation, operations.front());
        }
    }

    TEST_F(ApplierTest, WaitForCompletion) {
        const Timestamp timestamp(Seconds(123), 0);
        boost::mutex mutex;
        StatusWith<Timestamp> result = getDetectableErrorStatus();
        Applier::Operations operations;
        _applier.reset(new Applier(
                &getExecutor(),
                {BSON("ts" << timestamp)},
                [](OperationContext* txn, const BSONObj& operation) { return Status::OK(); },
                [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
            boost::lock_guard<boost::mutex> lock(mutex);
            result = theResult;
            operations = theOperations;
        }));

        getApplier()->start();
        getApplier()->wait();
        ASSERT_FALSE(getApplier()->isActive());

        boost::lock_guard<boost::mutex> lock(mutex);
        ASSERT_OK(result.getStatus());
        ASSERT_EQUALS(timestamp, result.getValue());
        ASSERT_TRUE(operations.empty());
    }

    TEST_F(ApplierTest, DestroyShouldBlockUntilInactive) {
        const Timestamp timestamp(Seconds(123), 0);
        unittest::Barrier barrier(2U);
        boost::mutex mutex;
        StatusWith<Timestamp> result = getDetectableErrorStatus();
        Applier::Operations operations;
        _applier.reset(new Applier(
                &getExecutor(),
                {BSON("ts" << timestamp)},
                [](OperationContext* txn, const BSONObj& operation) { return Status::OK(); },
                [&](const StatusWith<Timestamp>& theResult, const Operations& theOperations) {
            boost::lock_guard<boost::mutex> lock(mutex);
            result = theResult;
            operations = theOperations;
            barrier.countDownAndWait();
        }));

        getApplier()->start();
        barrier.countDownAndWait();
        _applier.reset();

        boost::lock_guard<boost::mutex> lock(mutex);
        ASSERT_OK(result.getStatus());
        ASSERT_EQUALS(timestamp, result.getValue());
        ASSERT_TRUE(operations.empty());
    }

    TEST_F(ApplierTest, ApplyOperationSuccessful) {
        // Bogus operations codes.
        Applier::Operations operationsToApply{
            BSON("op" << "a" << "ts" << Timestamp(Seconds(123), 0)),
            BSON("op" << "b" << "ts" << Timestamp(Seconds(456), 0)),
            BSON("op" << "c" << "ts" << Timestamp(Seconds(789), 0)),
        };
        boost::mutex mutex;
        StatusWith<Timestamp> result = getDetectableErrorStatus();
        bool areWritesReplicationOnOperationContext = true;
        bool isLockBatchWriter = false;
        Applier::Operations operationsApplied;
        Applier::Operations operationsOnCompletion;
        auto apply = [&](OperationContext* txn, const BSONObj& operation) {
            boost::lock_guard<boost::mutex> lock(mutex);
            areWritesReplicationOnOperationContext = txn->writesAreReplicated();
            isLockBatchWriter = txn->lockState()->isBatchWriter();
            operationsApplied.push_back(operation);
            return Status::OK();
        };
        auto callback = [&](const StatusWith<Timestamp>& theResult,
                            const Operations& theOperations) {
            boost::lock_guard<boost::mutex> lock(mutex);
            result = theResult;
            operationsOnCompletion = theOperations;
        };

        _applier.reset(new Applier(&getExecutor(), operationsToApply, apply, callback));
        _applier->start();
        _applier->wait();

        boost::lock_guard<boost::mutex> lock(mutex);
        ASSERT_FALSE(areWritesReplicationOnOperationContext);
        ASSERT_TRUE(isLockBatchWriter);
        ASSERT_EQUALS(operationsToApply.size(), operationsApplied.size());
        ASSERT_EQUALS(operationsToApply[0], operationsApplied[0]);
        ASSERT_EQUALS(operationsToApply[1], operationsApplied[1]);
        ASSERT_EQUALS(operationsToApply[2], operationsApplied[2]);
        ASSERT_OK(result.getStatus());
        ASSERT_EQUALS(operationsToApply[2]["ts"].timestamp(), result.getValue());
        ASSERT_TRUE(operationsOnCompletion.empty());
    }

    void ApplierTest::_testApplyOperationFailed(size_t opIndex, stdx::function<Status ()> fail) {
        // Bogus operations codes.
        Applier::Operations operationsToApply{
            BSON("op" << "a" << "ts" << Timestamp(Seconds(123), 0)),
            BSON("op" << "b" << "ts" << Timestamp(Seconds(456), 0)),
            BSON("op" << "c" << "ts" << Timestamp(Seconds(789), 0)),
        };
        boost::mutex mutex;
        StatusWith<Timestamp> result = getDetectableErrorStatus();
        Applier::Operations operationsApplied;
        Applier::Operations operationsOnCompletion;
        auto apply = [&](OperationContext* txn, const BSONObj& operation) {
            boost::lock_guard<boost::mutex> lock(mutex);
            if (operationsApplied.size() == opIndex) {
                return fail();
            }
            operationsApplied.push_back(operation);
            return Status::OK();
        };
        auto callback = [&](const StatusWith<Timestamp>& theResult,
                            const Operations& theOperations) {
            boost::lock_guard<boost::mutex> lock(mutex);
            result = theResult;
            operationsOnCompletion = theOperations;
        };

        _applier.reset(new Applier(&getExecutor(), operationsToApply, apply, callback));
        _applier->start();
        _applier->wait();

        boost::lock_guard<boost::mutex> lock(mutex);
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
        _testApplyOperationFailed(0U, []() {
            return Status(ErrorCodes::OperationFailed, "");
        });
    }

    TEST_F(ApplierTest, ApplyOperationThrowsExceptionOnFirstOperation) {
        _testApplyOperationFailed(0U, []() {
            uasserted(ErrorCodes::OperationFailed, "");
            MONGO_UNREACHABLE;
            return Status(ErrorCodes::InternalError, "unreachable");
        });
    }

    TEST_F(ApplierTest, ApplyOperationFailedOnSecondOperation) {
        _testApplyOperationFailed(1U, []() {
            return Status(ErrorCodes::OperationFailed, "");
        });
    }

    TEST_F(ApplierTest, ApplyOperationThrowsExceptionOnSecondOperation) {
        _testApplyOperationFailed(1U, []() {
            uasserted(ErrorCodes::OperationFailed, "");
            MONGO_UNREACHABLE;
            return Status(ErrorCodes::InternalError, "unreachable");
        });
    }

    TEST_F(ApplierTest, ApplyOperationFailedOnLastOperation) {
        _testApplyOperationFailed(2U, []() {
            return Status(ErrorCodes::OperationFailed, "");
        });
    }

    TEST_F(ApplierTest, ApplyOperationThrowsExceptionOnLastOperation) {
        _testApplyOperationFailed(2U, []() {
            uasserted(ErrorCodes::OperationFailed, "");
            MONGO_UNREACHABLE;
            return Status(ErrorCodes::InternalError, "unreachable");
        });
    }

} // namespace
