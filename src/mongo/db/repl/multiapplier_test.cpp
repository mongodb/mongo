/**
 *    Copyright 2016 MongoDB Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class MultiApplierTest : public executor::ThreadPoolExecutorTest {
public:
private:
    executor::ThreadPoolMock::Options makeThreadPoolMockOptions() const override;
    void setUp() override;
};

executor::ThreadPoolMock::Options MultiApplierTest::makeThreadPoolMockOptions() const {
    executor::ThreadPoolMock::Options options;
    options.onCreateThread = []() { Client::initThread("MultiApplierTest"); };
    return options;
}

void MultiApplierTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();

    launchExecutorThread();
}

Status applyOperation(MultiApplier::OperationPtrs*) {
    return Status::OK();
};

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
OplogEntry makeOplogEntry(int ts) {
    return OplogEntry(OpTime(Timestamp(ts, 1), 1),  // optime
                      1LL,                          // hash
                      OpTypeEnum::kNoop,            // op type
                      NamespaceString("a.a"),       // namespace
                      boost::none,                  // uuid
                      boost::none,                  // fromMigrate
                      OplogEntry::kOplogVersion,    // version
                      BSONObj(),                    // o
                      boost::none,                  // o2
                      {},                           // sessionInfo
                      boost::none,                  // upsert
                      boost::none,                  // wall clock time
                      boost::none,                  // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none);  // post-image optime
}

TEST_F(MultiApplierTest, InvalidConstruction) {
    const MultiApplier::Operations operations{makeOplogEntry(123)};
    auto multiApply = [](OperationContext*,
                         MultiApplier::Operations,
                         MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        return Status(ErrorCodes::InternalError, "not implemented");
    };
    auto callback = [](const Status&) {};

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(nullptr, operations, applyOperation, multiApply, callback),
        AssertionException,
        ErrorCodes::BadValue,
        "null replication executor");

    // Empty list of operations.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), MultiApplier::Operations(), applyOperation, multiApply, callback),
        AssertionException,
        ErrorCodes::BadValue,
        "empty list of operations");

    // Invalid apply operation function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), operations, MultiApplier::ApplyOperationFn(), multiApply, callback),
        AssertionException,
        ErrorCodes::BadValue,
        "apply operation function cannot be null");

    // Invalid multiApply operation function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), operations, applyOperation, MultiApplier::MultiApplyFn(), callback),
        AssertionException,
        ErrorCodes::BadValue,
        "multi apply function cannot be null");

    // Invalid callback function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), operations, applyOperation, multiApply, MultiApplier::CallbackFn()),
        AssertionException,
        ErrorCodes::BadValue,
        "callback function cannot be null");
}

TEST_F(MultiApplierTest, MultiApplierTransitionsDirectlyToCompleteIfShutdownBeforeStarting) {
    const MultiApplier::Operations operations{makeOplogEntry(123)};

    auto multiApply = [](OperationContext*,
                         MultiApplier::Operations,
                         MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> { return OpTime(); };
    auto callback = [](const Status&) {};

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_EQUALS(MultiApplier::State::kPreStart, multiApplier.getState_forTest());

    multiApplier.shutdown();
    ASSERT_EQUALS(MultiApplier::State::kComplete, multiApplier.getState_forTest());
}

TEST_F(MultiApplierTest, MultiApplierInvokesCallbackWithCallbackCanceledStatusUponCancellation) {
    const MultiApplier::Operations operations{makeOplogEntry(123)};

    bool multiApplyInvoked = false;
    auto multiApply = [&](OperationContext* opCtx,
                          MultiApplier::Operations operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_EQUALS(MultiApplier::State::kPreStart, multiApplier.getState_forTest());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Executor cannot run multiApply callback while we are on the network thread.
        ASSERT_OK(multiApplier.startup());
        ASSERT_EQUALS(MultiApplier::State::kRunning, multiApplier.getState_forTest());

        multiApplier.shutdown();
        ASSERT_EQUALS(MultiApplier::State::kShuttingDown, multiApplier.getState_forTest());

        net->runReadyNetworkOperations();
    }
    multiApplier.join();
    ASSERT_EQUALS(MultiApplier::State::kComplete, multiApplier.getState_forTest());

    ASSERT_FALSE(multiApplyInvoked);

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, callbackResult);
}

TEST_F(MultiApplierTest, MultiApplierPassesMultiApplyErrorToCallback) {
    const MultiApplier::Operations operations{makeOplogEntry(123)};

    bool multiApplyInvoked = false;
    Status multiApplyError(ErrorCodes::OperationFailed, "multi apply failed");
    auto multiApply = [&](OperationContext*,
                          MultiApplier::Operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        return multiApplyError;
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_TRUE(multiApplyInvoked);

    ASSERT_EQUALS(multiApplyError, callbackResult);
}

TEST_F(MultiApplierTest, MultiApplierCatchesMultiApplyExceptionAndConvertsToCallbackStatus) {
    const MultiApplier::Operations operations{makeOplogEntry(123)};

    bool multiApplyInvoked = false;
    Status multiApplyError(ErrorCodes::OperationFailed, "multi apply failed");
    auto multiApply = [&](OperationContext* opCtx,
                          MultiApplier::Operations operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        uassertStatusOK(multiApplyError);
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_TRUE(multiApplyInvoked);

    ASSERT_EQUALS(multiApplyError, callbackResult);
}

TEST_F(
    MultiApplierTest,
    MultiApplierProvidesOperationContextToMultiApplyFunctionButDisposesBeforeInvokingFinishCallback) {
    const MultiApplier::Operations operations{makeOplogEntry(123)};

    OperationContext* multiApplyTxn = nullptr;
    MultiApplier::Operations operationsToApply;
    auto multiApply = [&](OperationContext* opCtx,
                          MultiApplier::Operations operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyTxn = opCtx;
        operationsToApply = operations;
        return operationsToApply.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    OperationContext* callbackTxn = nullptr;
    auto callback = [&](const Status& result) {
        callbackResult = result;
        callbackTxn = cc().getOperationContext();
    };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_TRUE(multiApplyTxn);
    ASSERT_EQUALS(1U, operationsToApply.size());
    ASSERT_BSONOBJ_EQ(operations[0].raw, operationsToApply[0].raw);

    ASSERT_OK(callbackResult);
    ASSERT_FALSE(callbackTxn);
}

class SharedCallbackState {
    MONGO_DISALLOW_COPYING(SharedCallbackState);

public:
    explicit SharedCallbackState(bool* sharedCallbackStateDestroyed)
        : _sharedCallbackStateDestroyed(sharedCallbackStateDestroyed) {}
    ~SharedCallbackState() {
        *_sharedCallbackStateDestroyed = true;
    }

private:
    bool* _sharedCallbackStateDestroyed;
};

TEST_F(MultiApplierTest, MultiApplierResetsOnCompletionCallbackFunctionPointerUponCompletion) {
    bool sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>(&sharedCallbackStateDestroyed);

    const MultiApplier::Operations operations{makeOplogEntry(123)};

    auto multiApply = [&](OperationContext*,
                          MultiApplier::Operations operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();

    MultiApplier multiApplier(
        &getExecutor(),
        operations,
        applyOperation,
        multiApply,
        [&callbackResult, sharedCallbackData](const Status& result) { callbackResult = result; });

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_OK(callbackResult);
    // Shared state should be destroyed when applier is finished.
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

}  // namespace
