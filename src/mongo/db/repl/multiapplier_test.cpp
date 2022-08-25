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

#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class MultiApplierTest : public executor::ThreadPoolExecutorTest,
                         ScopedGlobalServiceContextForTest {
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

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
OplogEntry makeOplogEntry(int ts) {
    return {DurableOplogEntry(OpTime(Timestamp(ts, 1), 1),  // optime
                              OpTypeEnum::kNoop,            // op type
                              NamespaceString("a.a"),       // namespace
                              boost::none,                  // uuid
                              boost::none,                  // fromMigrate
                              OplogEntry::kOplogVersion,    // version
                              BSONObj(),                    // o
                              boost::none,                  // o2
                              {},                           // sessionInfo
                              boost::none,                  // upsert
                              Date_t(),                     // wall clock time
                              {},                           // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

TEST_F(MultiApplierTest, InvalidConstruction) {
    const std::vector<OplogEntry> operations{makeOplogEntry(123)};
    auto multiApply = [](OperationContext*, std::vector<OplogEntry>) -> StatusWith<OpTime> {
        return Status(ErrorCodes::InternalError, "not implemented");
    };
    auto callback = [](const Status&) {};

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(MultiApplier(nullptr, operations, multiApply, callback),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "null replication executor");

    // Empty list of operations.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(&getExecutor(), std::vector<OplogEntry>(), multiApply, callback),
        AssertionException,
        ErrorCodes::BadValue,
        "empty list of operations");

    // Invalid multiApply operation function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(&getExecutor(), operations, MultiApplier::MultiApplyFn(), callback),
        AssertionException,
        ErrorCodes::BadValue,
        "multi apply function cannot be null");

    // Invalid callback function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(&getExecutor(), operations, multiApply, MultiApplier::CallbackFn()),
        AssertionException,
        ErrorCodes::BadValue,
        "callback function cannot be null");
}

TEST_F(MultiApplierTest, MultiApplierTransitionsDirectlyToCompleteIfShutdownBeforeStarting) {
    const std::vector<OplogEntry> operations{makeOplogEntry(123)};

    auto multiApply = [](OperationContext*, std::vector<OplogEntry>) -> StatusWith<OpTime> {
        return OpTime();
    };
    auto callback = [](const Status&) {};

    MultiApplier multiApplier(&getExecutor(), operations, multiApply, callback);
    ASSERT_EQUALS(MultiApplier::State::kPreStart, multiApplier.getState_forTest());

    multiApplier.shutdown();
    ASSERT_EQUALS(MultiApplier::State::kComplete, multiApplier.getState_forTest());
}

TEST_F(MultiApplierTest, MultiApplierInvokesCallbackWithCallbackCanceledStatusUponCancellation) {
    const std::vector<OplogEntry> operations{makeOplogEntry(123)};

    bool multiApplyInvoked = false;
    auto multiApply = [&](OperationContext* opCtx,
                          std::vector<OplogEntry> operations) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, multiApply, callback);
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
    const std::vector<OplogEntry> operations{makeOplogEntry(123)};

    bool multiApplyInvoked = false;
    Status multiApplyError(ErrorCodes::OperationFailed, "multi apply failed");
    auto multiApply = [&](OperationContext*, std::vector<OplogEntry>) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        return multiApplyError;
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, multiApply, callback);
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
    const std::vector<OplogEntry> operations{makeOplogEntry(123)};

    bool multiApplyInvoked = false;
    Status multiApplyError(ErrorCodes::OperationFailed, "multi apply failed");
    auto multiApply = [&](OperationContext* opCtx,
                          std::vector<OplogEntry> operations) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        uassertStatusOK(multiApplyError);
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, multiApply, callback);
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
    const std::vector<OplogEntry> operations{makeOplogEntry(123)};

    OperationContext* multiApplyTxn = nullptr;
    std::vector<OplogEntry> operationsToApply;
    auto multiApply = [&](OperationContext* opCtx,
                          std::vector<OplogEntry> operations) -> StatusWith<OpTime> {
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

    MultiApplier multiApplier(&getExecutor(), operations, multiApply, callback);
    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_TRUE(multiApplyTxn);
    ASSERT_EQUALS(1U, operationsToApply.size());
    ASSERT_BSONOBJ_EQ(operations[0].getEntry().toBSON(), operationsToApply[0].getEntry().toBSON());

    ASSERT_OK(callbackResult);
    ASSERT_FALSE(callbackTxn);
}

class SharedCallbackState {
    SharedCallbackState(const SharedCallbackState&) = delete;
    SharedCallbackState& operator=(const SharedCallbackState&) = delete;

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

    const std::vector<OplogEntry> operations{makeOplogEntry(123)};

    auto multiApply = [&](OperationContext*,
                          std::vector<OplogEntry> operations) -> StatusWith<OpTime> {
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();

    MultiApplier multiApplier(
        &getExecutor(),
        operations,
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
