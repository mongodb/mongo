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

#include "mongo/client/fetcher.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/error_labels.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_test_utils.h"

#include <list>
#include <memory>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace {

using namespace mongo;
using executor::RemoteCommandRequest;
using executor::TaskExecutor;

using ResponseStatus = TaskExecutor::ResponseStatus;

const HostAndPort source("localhost", -1);
const BSONObj findCmdObj = BSON("find" << "coll");

class FetcherTest : public executor::ThreadPoolExecutorTest {
public:
    FetcherTest();
    void clear();

    enum class ReadyQueueState { kEmpty, kHasReadyRequests };

    enum class FetcherState { kInactive, kActive };

    // Calls scheduleSuccessfulResponse/scheduleErrorResponse + finishProcessingNetworkResponse
    void processNetworkResponse(const BSONObj& obj,
                                ReadyQueueState readyQueueStateAfterProcessing,
                                FetcherState fetcherStateAfterProcessing);
    void processNetworkResponse(ResponseStatus,
                                ReadyQueueState readyQueueStateAfterProcessing,
                                FetcherState fetcherStateAfterProcessing);
    void processNetworkResponse(const BSONObj& obj,
                                Milliseconds elapsed,
                                ReadyQueueState readyQueueStateAfterProcessing,
                                FetcherState fetcherStateAfterProcessing);

    void finishProcessingNetworkResponse(ReadyQueueState readyQueueStateAfterProcessing,
                                         FetcherState fetcherStateAfterProcessing);

protected:
    Fetcher::CallbackFn makeCallback();

    void setUp() override;
    void tearDown() override;

    Status status;
    boost::optional<HostAndPort> target;
    std::vector<std::string> errorLabels;
    CursorId cursorId;
    NamespaceString nss;
    Fetcher::Documents documents;
    Milliseconds elapsedMillis;
    bool first;
    Fetcher::NextAction nextAction;
    std::unique_ptr<Fetcher> fetcher;
    // Called at end of _callback
    Fetcher::CallbackFn callbackHook;

private:
    void _callback(const Fetcher::QueryResponseStatus& result,
                   Fetcher::NextAction* nextAction,
                   BSONObjBuilder* getMoreBob);
};

FetcherTest::FetcherTest()
    : status(getDetectableErrorStatus()), cursorId(-1), nextAction(Fetcher::NextAction::kInvalid) {}

Fetcher::CallbackFn FetcherTest::makeCallback() {
    return [this](const auto& x, const auto& y, const auto& z) {
        return this->_callback(x, y, z);
    };
}

void FetcherTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    clear();
    callbackHook = Fetcher::CallbackFn();
    fetcher = std::make_unique<Fetcher>(&getExecutor(),
                                        source,
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        findCmdObj,
                                        makeCallback());
    launchExecutorThread();
}

void FetcherTest::tearDown() {
    shutdownExecutorThread();
    joinExecutorThread();
    // Executor may still invoke fetcher's callback before shutting down.
    fetcher.reset();
}

void FetcherTest::clear() {
    status = getDetectableErrorStatus();
    cursorId = -1;
    nss = NamespaceString::kEmpty;
    documents.clear();
    elapsedMillis = Milliseconds(0);
    first = false;
    nextAction = Fetcher::NextAction::kInvalid;
}

void FetcherTest::processNetworkResponse(const BSONObj& obj,
                                         ReadyQueueState readyQueueStateAfterProcessing,
                                         FetcherState fetcherStateAfterProcessing) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    getNet()->scheduleSuccessfulResponse(obj);
    finishProcessingNetworkResponse(readyQueueStateAfterProcessing, fetcherStateAfterProcessing);
}

void FetcherTest::processNetworkResponse(const BSONObj& obj,
                                         Milliseconds elapsed,
                                         ReadyQueueState readyQueueStateAfterProcessing,
                                         FetcherState fetcherStateAfterProcessing) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    getNet()->scheduleSuccessfulResponse(ResponseStatus::make_forTest(obj, elapsed));
    finishProcessingNetworkResponse(readyQueueStateAfterProcessing, fetcherStateAfterProcessing);
}

void FetcherTest::processNetworkResponse(ResponseStatus rs,
                                         ReadyQueueState readyQueueStateAfterProcessing,
                                         FetcherState fetcherStateAfterProcessing) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    getNet()->scheduleErrorResponse(rs);
    finishProcessingNetworkResponse(readyQueueStateAfterProcessing, fetcherStateAfterProcessing);
}

void FetcherTest::finishProcessingNetworkResponse(ReadyQueueState readyQueueStateAfterProcessing,
                                                  FetcherState fetcherStateAfterProcessing) {
    clear();
    ASSERT_TRUE(fetcher->isActive());
    getNet()->runReadyNetworkOperations();
    ASSERT_EQUALS(readyQueueStateAfterProcessing == ReadyQueueState::kHasReadyRequests,
                  getNet()->hasReadyRequests());
    ASSERT_EQUALS(fetcherStateAfterProcessing == FetcherState::kActive, fetcher->isActive());
}

void FetcherTest::_callback(const Fetcher::QueryResponseStatus& result,
                            Fetcher::NextAction* nextActionFromFetcher,
                            BSONObjBuilder* getMoreBob) {
    status = result.getStatus();
    if (result.isOK()) {
        const Fetcher::QueryResponse& batchData = result.getValue();
        cursorId = batchData.cursorId;
        nss = batchData.nss;
        documents = batchData.documents;
        elapsedMillis = duration_cast<Milliseconds>(batchData.elapsed);
        first = batchData.first;
        errorLabels.clear();
    } else {
        auto labels = result.getErrorLabels();
        errorLabels.assign(labels.begin(), labels.end());
    }

    target = result.getOrigin();

    if (callbackHook) {
        callbackHook(result, nextActionFromFetcher, getMoreBob);
    }

    if (nextActionFromFetcher) {
        nextAction = *nextActionFromFetcher;
    }
}

void unreachableCallback(const Fetcher::QueryResponseStatus& fetchResult,
                         Fetcher::NextAction* nextAction,
                         BSONObjBuilder* getMoreBob) {
    FAIL("should not reach here");
}

void doNothingCallback(const Fetcher::QueryResponseStatus& fetchResult,
                       Fetcher::NextAction* nextAction,
                       BSONObjBuilder* getMoreBob) {}

TEST_F(FetcherTest, InvalidConstruction) {
    TaskExecutor& executor = getExecutor();

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(nullptr,
                                        source,
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        findCmdObj,
                                        unreachableCallback),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "task executor cannot be null");

    // Empty source.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(&executor,
                                        HostAndPort(),
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        findCmdObj,
                                        unreachableCallback),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "source in remote command request cannot be empty");

    // Empty database name.
    ASSERT_THROWS_CODE_AND_WHAT(
        Fetcher(&executor, source, DatabaseName(), findCmdObj, unreachableCallback),
        AssertionException,
        ErrorCodes::BadValue,
        "database name in remote command request cannot be empty");

    // Empty command object.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(&executor,
                                        source,
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        BSONObj(),
                                        unreachableCallback),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "command object in remote command request cannot be empty");

    // Callback function cannot be null.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(&executor,
                                        source,
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        findCmdObj,
                                        Fetcher::CallbackFn()),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "callback function cannot be null");

    // Retry strategy for first command cannot be null.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(&executor,
                                        source,
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        findCmdObj,
                                        unreachableCallback,
                                        rpc::makeEmptyMetadata(),
                                        RemoteCommandRequest::kNoTimeout,
                                        RemoteCommandRequest::kNoTimeout,
                                        std::unique_ptr<DefaultRetryStrategy>()),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "retry strategy cannot be null");
}

TEST_F(FetcherTest, FetcherCompletionFutureBecomesReadyAfterCompletingWork) {
    // Used to check that the future continuation was run.
    int i = 0;

    ASSERT_FALSE(fetcher->onCompletion().isReady());

    ASSERT_OK(fetcher->schedule());

    ASSERT_TRUE(fetcher->isActive());
    ASSERT_FALSE(fetcher->onCompletion().isReady());

    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());

    auto fut = fetcher->onCompletion().thenRunOn(getExecutorPtr()).then([&, this] {
        ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
        i++;
    });

    fut.wait();
    ASSERT_EQUALS(i, 1);
}

TEST_F(FetcherTest, FetcherCompletionFutureBecomesReadyEvenWhenWorkIsInterruptedByShutdown) {
    // Used to check that the future continuation was run.
    int i = 0;

    ASSERT_FALSE(fetcher->onCompletion().isReady());

    ASSERT_OK(fetcher->schedule());

    ASSERT_TRUE(fetcher->isActive());
    ASSERT_FALSE(fetcher->onCompletion().isReady());

    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());

    fetcher->shutdown();

    // On shutdown, we expect that the async fetcher will not be stuck waiting.
    auto fut = fetcher->onCompletion().thenRunOn(getExecutorPtr()).then([&, this] {
        ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
        i++;
    });

    fut.wait();
    ASSERT_EQUALS(i, 1);
}

TEST_F(FetcherTest, FetcherCompletionFutureBecomesReadyWhenFetcherIsShutdownBeforeSchedulingWork) {
    // Used to check that the future continuation was run.
    int i = 0;

    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());
    ASSERT_FALSE(fetcher->onCompletion().isReady());
    ASSERT_FALSE(fetcher->isActive());

    fetcher->shutdown();

    // On shutdown, we expect that the async fetcher will not be stuck waiting.
    auto fut = fetcher->onCompletion().thenRunOn(getExecutorPtr()).then([&, this] {
        ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
        i++;
    });

    fut.wait();
    ASSERT_EQUALS(i, 1);
}

// Command object can refer to any command that returns a cursor. This
// includes listIndexes and listCollections.
TEST_F(FetcherTest, NonFindCommand) {
    TaskExecutor& executor = getExecutor();

    Fetcher f1(&executor,
               source,
               DatabaseName::createDatabaseName_forTest(boost::none, "db"),
               BSON("listIndexes" << "coll"),
               unreachableCallback);
    Fetcher f2(&executor,
               source,
               DatabaseName::createDatabaseName_forTest(boost::none, "db"),
               BSON("listCollections" << 1),
               unreachableCallback);
    Fetcher f3(&executor,
               source,
               DatabaseName::createDatabaseName_forTest(boost::none, "db"),
               BSON("a" << 1),
               unreachableCallback);
}

TEST_F(FetcherTest, RemoteCommandRequestShouldContainCommandParametersPassedToConstructor) {
    auto metadataObj = BSON("x" << 1);
    Milliseconds timeout(8000);

    fetcher = std::make_unique<Fetcher>(&getExecutor(),
                                        source,
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        findCmdObj,
                                        doNothingCallback,
                                        metadataObj,
                                        timeout);

    ASSERT_EQUALS(source, fetcher->getSource());
    ASSERT_BSONOBJ_EQ(findCmdObj, fetcher->getCommandObject());
    ASSERT_BSONOBJ_EQ(metadataObj, fetcher->getMetadataObject());

    ASSERT_OK(fetcher->schedule());

    auto net = getNet();
    executor::RemoteCommandRequest request;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        ASSERT_EQUALS(timeout, request.timeout);
    }

    ASSERT_EQUALS(source, request.target);
    ASSERT_BSONOBJ_EQ(findCmdObj, request.cmdObj);
    ASSERT_BSONOBJ_EQ(metadataObj, request.metadata);
    ASSERT_EQUALS(timeout, request.timeout);
}

TEST_F(FetcherTest, GetDiagnosticString) {
    ASSERT_FALSE(fetcher->getDiagnosticString().empty());
}

TEST_F(FetcherTest, IsActiveAfterSchedule) {
    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());
    ASSERT_FALSE(fetcher->isActive());
    ASSERT_OK(fetcher->schedule());
    ASSERT_TRUE(fetcher->isActive());
    ASSERT_EQUALS(Fetcher::State::kRunning, fetcher->getState_forTest());
}

TEST_F(FetcherTest, ScheduleWhenActive) {
    ASSERT_OK(fetcher->schedule());
    ASSERT_TRUE(fetcher->isActive());
    ASSERT_EQUALS(ErrorCodes::InternalError, fetcher->schedule());
}

TEST_F(FetcherTest, CancelWithoutSchedule) {
    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());
    ASSERT_FALSE(fetcher->isActive());
    fetcher->shutdown();
    ASSERT_FALSE(fetcher->isActive());
    ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
}

TEST_F(FetcherTest, WaitWithoutSchedule) {
    ASSERT_FALSE(fetcher->isActive());
    ASSERT_OK(fetcher->join(Interruptible::notInterruptible()));
    ASSERT_FALSE(fetcher->isActive());
}

TEST_F(FetcherTest, ShutdownBeforeSchedule) {
    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());
    getExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, fetcher->schedule());
    ASSERT_FALSE(fetcher->isActive());
    ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
}

TEST_F(FetcherTest, ScheduleAndCancel) {
    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());

    ASSERT_OK(fetcher->schedule());
    ASSERT_EQUALS(Fetcher::State::kRunning, fetcher->getState_forTest());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        assertRemoteCommandNameEquals("find", net->scheduleSuccessfulResponse(BSON("ok" << 1)));
    }

    fetcher->shutdown();
    ASSERT_EQUALS(Fetcher::State::kShuttingDown, fetcher->getState_forTest());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        finishProcessingNetworkResponse(ReadyQueueState::kEmpty, FetcherState::kInactive);
    }

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status.code());
    ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
}


TEST_F(FetcherTest, ScheduleAndCancelDueToJoinInterruption) {
    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());

    ASSERT_OK(fetcher->schedule());
    ASSERT_EQUALS(Fetcher::State::kRunning, fetcher->getState_forTest());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        assertRemoteCommandNameEquals("find", net->scheduleSuccessfulResponse(BSON("ok" << 1)));
    }

    ASSERT_TRUE(fetcher->isActive());
    ASSERT_EQUALS(Fetcher::State::kRunning, fetcher->getState_forTest());

    DummyInterruptible interruptible;
    ASSERT_EQ(fetcher->join(&interruptible), ErrorCodes::Interrupted);

    // To make this test deterministic, we need the Fetcher to already be shut down so it doesn't
    // attempt to process the scheduled response. Normally Fetcher::join() would be solely
    // responsible for calling Fetcher::shutdown().
    fetcher->shutdown();

    // The finishProcessingNetworkResponseThread is needed to prevent the main test thread from
    // blocking on the NetworkInterfaceMock.
    stdx::thread finishProcessingNetworkResponseThread([&]() {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        getNet()->runReadyNetworkOperations();
    });

    // We destroy the Fetcher before shutting down the task executor to reflect what would
    // ordinarily happen after Fetcher::join() returns an error Status from the Interruptible being
    // interrupted.
    fetcher.reset();
    finishProcessingNetworkResponseThread.join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status.code());
}

TEST_F(FetcherTest, ScheduleButShutdown) {
    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());

    ASSERT_OK(fetcher->schedule());
    ASSERT_EQUALS(Fetcher::State::kRunning, fetcher->getState_forTest());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        auto noi = net->getNextReadyRequest();
        assertRemoteCommandNameEquals("find", noi->getRequest());
        net->blackHole(noi);
    }

    ASSERT_TRUE(fetcher->isActive());
    ASSERT_EQUALS(Fetcher::State::kRunning, fetcher->getState_forTest());

    shutdownExecutorThread();

    ASSERT_OK(fetcher->join(Interruptible::notInterruptible()));
    ASSERT_FALSE(fetcher->isActive());

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status.code());
    ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
}

TEST_F(FetcherTest, ScheduleAfterCompletionReturnsShutdownInProgress) {
    ASSERT_EQUALS(Fetcher::State::kPreStart, fetcher->getState_forTest());
    ASSERT_OK(fetcher->schedule());
    auto rs = ResponseStatus::make_forTest(
        Status(ErrorCodes::OperationFailed, "find command failed"), Milliseconds(0));
    processNetworkResponse(rs, ReadyQueueState::kEmpty, FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status.code());

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, fetcher->schedule());
    ASSERT_EQUALS(Fetcher::State::kComplete, fetcher->getState_forTest());
}

TEST_F(FetcherTest, FindCommandFailed1) {
    ASSERT_OK(fetcher->schedule());
    auto rs =
        ResponseStatus::make_forTest(Status(ErrorCodes::BadValue, "bad hint"), Milliseconds(0));
    processNetworkResponse(rs, ReadyQueueState::kEmpty, FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_EQUALS("bad hint", status.reason());
}

TEST_F(FetcherTest, FindCommandFailed2) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                     << "bad hint"
                                     << "code" << int(ErrorCodes::BadValue)),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_EQUALS("bad hint", status.reason());
}

TEST_F(FetcherTest, FindCommandFailedWithErrorLabels) {
    auto responseErrorLabels =
        std::array{ErrorLabel::kSystemOverloadedError, ErrorLabel::kRetryableError};
    BSONArrayBuilder labelArray;
    for (const auto& label : responseErrorLabels) {
        labelArray << label;
    }

    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                     << "bad hint" << kErrorLabelsFieldName << labelArray.arr()
                                     << "code" << int(ErrorCodes::AdmissionQueueOverflow)),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::AdmissionQueueOverflow, status.code());
    ASSERT_EQUALS(source, target);
    ASSERT(std::ranges::equal(responseErrorLabels, errorLabels));
}

TEST_F(FetcherTest, CursorFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("ok" << 1), ReadyQueueState::kEmpty, FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor' field");
}

TEST_F(FetcherTest, CursorNotAnObject) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << 123 << "ok" << 1), ReadyQueueState::kEmpty, FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor' field must be an object");
}

TEST_F(FetcherTest, CursorIdFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("ns" << "db.coll"
                                                      << "firstBatch" << BSONArray())
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.id' field");
}

TEST_F(FetcherTest, CursorIdNotLongNumber) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 123.1 << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSONArray())
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.id' field must be");
    ASSERT_EQ((int)Fetcher::NextAction::kInvalid, (int)nextAction);
}

TEST_F(FetcherTest, NamespaceFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 123LL << "firstBatch" << BSONArray()) << "ok" << 1),
        ReadyQueueState::kEmpty,
        FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.ns' field");
}

TEST_F(FetcherTest, NamespaceNotAString) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor"
                                << BSON("id" << 123LL << "ns" << 123 << "firstBatch" << BSONArray())
                                << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' field must be a string");
}

TEST_F(FetcherTest, NamespaceEmpty) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 123LL << "ns"
                                                      << ""
                                                      << "firstBatch" << BSONArray())
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
}

TEST_F(FetcherTest, NamespaceMissingCollectionName) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 123LL << "ns"
                                                      << "db."
                                                      << "firstBatch" << BSONArray())
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
}

TEST_F(FetcherTest, FirstBatchFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll")
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.firstBatch' field");
}

TEST_F(FetcherTest, FirstBatchNotAnArray) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << 123)
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.firstBatch' field must be an array");
}

TEST_F(FetcherTest, FirstBatchArrayContainsNonObject) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(8))
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "found non-object");
    ASSERT_STRING_CONTAINS(status.reason(), "in 'cursor.firstBatch' field");
}

TEST_F(FetcherTest, FirstBatchEmptyArray) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSONArray())
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_TRUE(documents.empty());
}

TEST_F(FetcherTest, FetchOneDocument) {
    ASSERT_OK(fetcher->schedule());
    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
}

TEST_F(FetcherTest, SetNextActionToContinueWhenNextBatchIsNotAvailable) {
    ASSERT_OK(fetcher->schedule());
    const BSONObj doc = BSON("_id" << 1);
    callbackHook = [](const StatusWith<Fetcher::QueryResponse>& fetchResult,
                      Fetcher::NextAction* nextAction,
                      BSONObjBuilder* getMoreBob) {
        ASSERT_OK(fetchResult.getStatus());
        Fetcher::QueryResponse batchData = fetchResult.getValue();

        ASSERT(nextAction);
        *nextAction = Fetcher::NextAction::kGetMore;
        ASSERT_FALSE(getMoreBob);
    };
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
}

void appendGetMoreRequest(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                          Fetcher::NextAction* nextAction,
                          BSONObjBuilder* getMoreBob) {
    if (!getMoreBob) {
        return;
    }
    const auto& batchData = fetchResult.getValue();
    getMoreBob->append("getMore", batchData.cursorId);
    getMoreBob->append("collection", batchData.nss.coll());
}

TEST_F(FetcherTest, FetchMultipleBatches) {
    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           Milliseconds(100),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
    ASSERT_EQUALS(elapsedMillis, Milliseconds(100));
    ASSERT_TRUE(first);
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    const BSONObj doc2 = BSON("_id" << 2);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "nextBatch" << BSON_ARRAY(doc2))
                                         << "ok" << 1),
                           Milliseconds(200),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc2, documents.front());
    ASSERT_EQUALS(elapsedMillis, Milliseconds(200));
    ASSERT_FALSE(first);
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    const BSONObj doc3 = BSON("_id" << 3);

    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "nextBatch" << BSON_ARRAY(doc3))
                                         << "ok" << 1),
                           Milliseconds(300),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);

    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc3, documents.front());
    ASSERT_EQUALS(elapsedMillis, Milliseconds(300));
    ASSERT_FALSE(first);
    ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
}

TEST_F(FetcherTest, ScheduleGetMoreAndCancel) {
    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    const BSONObj doc2 = BSON("_id" << 2);
    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "nextBatch" << BSON_ARRAY(doc2))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc2, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    fetcher->shutdown();

    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        finishProcessingNetworkResponse(ReadyQueueState::kEmpty, FetcherState::kInactive);
    }

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status);
}

TEST_F(FetcherTest, CancelDuringCallbackPutsFetcherInShutdown) {
    Status fetchStatus1 = Status(ErrorCodes::InternalError, "error");
    Status fetchStatus2 = Status(ErrorCodes::InternalError, "error");
    callbackHook = [&](const StatusWith<Fetcher::QueryResponse>& fetchResult,
                       Fetcher::NextAction* nextAction,
                       BSONObjBuilder* getMoreBob) {
        if (!getMoreBob) {
            fetchStatus2 = fetchResult.getStatus();
            return;
        }
        const auto& batchData = fetchResult.getValue();
        getMoreBob->append("getMore", batchData.cursorId);
        getMoreBob->append("collection", batchData.nss.coll());

        fetchStatus1 = fetchResult.getStatus();
        fetcher->shutdown();
    };
    fetcher->schedule().transitional_ignore();
    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kInactive);

    ASSERT_FALSE(fetcher->isActive());
    ASSERT_OK(fetchStatus1);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, fetchStatus2);
}

TEST_F(FetcherTest, ScheduleGetMoreButShutdown) {
    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    const BSONObj doc2 = BSON("_id" << 2);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "nextBatch" << BSON_ARRAY(doc2))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc2, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    getExecutor().shutdown();

    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status);
}


TEST_F(FetcherTest, EmptyGetMoreRequestAfterFirstBatchMakesFetcherInactiveAndKillsCursor) {
    unittest::LogCaptureGuard logs;

    callbackHook = [](const StatusWith<Fetcher::QueryResponse>& fetchResult,
                      Fetcher::NextAction* nextAction,
                      BSONObjBuilder* getMoreBob) {
    };

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kInactive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    executor::RemoteCommandRequest request;
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_TRUE(getNet()->hasReadyRequests());
        auto noi = getNet()->getNextReadyRequest();
        request = noi->getRequest();
    }

    ASSERT_EQUALS(nss.dbName(), request.dbname);
    auto&& cmdObj = request.cmdObj;
    auto firstElement = cmdObj.firstElement();
    ASSERT_EQUALS("killCursors", firstElement.fieldNameStringData());
    ASSERT_EQUALS(nss.coll(), firstElement.String());
    ASSERT_EQUALS(mongo::BSONType::array, cmdObj["cursors"].type());
    auto cursors = cmdObj["cursors"].Array();
    ASSERT_EQUALS(1U, cursors.size());
    ASSERT_EQUALS(cursorId, cursors.front().numberLong());

    // killCursors command request will be canceled by executor on shutdown.
    tearDown();
    ASSERT_EQUALS(1,
                  logs.countBSONContainingSubset(BSON("msg" << "killCursors command task failed")));
}

void setNextActionToNoAction(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                             Fetcher::NextAction* nextAction,
                             BSONObjBuilder* getMoreBob) {
    *nextAction = Fetcher::NextAction::kNoAction;
}

TEST_F(FetcherTest, UpdateNextActionAfterSecondBatch) {
    unittest::LogCaptureGuard logs;

    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    const BSONObj doc2 = BSON("_id" << 2);

    callbackHook = setNextActionToNoAction;

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "nextBatch" << BSON_ARRAY(doc2))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kInactive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc2, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);

    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();

        ASSERT_EQUALS(nss.dbName(), request.dbname);
        auto&& cmdObj = request.cmdObj;
        auto firstElement = cmdObj.firstElement();
        ASSERT_EQUALS("killCursors", firstElement.fieldNameStringData());
        ASSERT_EQUALS(nss.coll(), firstElement.String());
        ASSERT_EQUALS(mongo::BSONType::array, cmdObj["cursors"].type());
        auto cursors = cmdObj["cursors"].Array();
        ASSERT_EQUALS(1U, cursors.size());
        ASSERT_EQUALS(cursorId, cursors.front().numberLong());

        // Failed killCursors command response should be logged.
        getNet()->scheduleSuccessfulResponse(
            noi, ResponseStatus::make_forTest(BSON("ok" << false), Milliseconds(0)));
        getNet()->runReadyNetworkOperations();
    }

    ASSERT_EQUALS(
        1,
        logs.countBSONContainingSubset(BSON("msg" << "killCursors command failed"
                                                  << "attr" << BSON("error" << "UnknownError: "))));
}

/**
 * This will be invoked twice before the fetcher returns control to the task executor.
 */
void shutdownDuringSecondBatch(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                               Fetcher::NextAction* nextAction,
                               BSONObjBuilder* getMoreBob,
                               const BSONObj& doc2,
                               TaskExecutor* executor,
                               bool* isShutdownCalled) {
    if (*isShutdownCalled) {
        return;
    }

    // First time during second batch
    ASSERT_OK(fetchResult.getStatus());
    Fetcher::QueryResponse batchData = fetchResult.getValue();
    ASSERT_EQUALS(1U, batchData.documents.size());
    ASSERT_BSONOBJ_EQ(doc2, batchData.documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == *nextAction);
    ASSERT(getMoreBob);
    getMoreBob->append("getMore", batchData.cursorId);
    getMoreBob->append("collection", batchData.nss.coll());

    executor->shutdown();
    *isShutdownCalled = true;
}

TEST_F(FetcherTest, ShutdownDuringSecondBatch) {
    unittest::LogCaptureGuard logs;

    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    const BSONObj doc2 = BSON("_id" << 2);

    bool isShutdownCalled = false;
    callbackHook = [this, doc2, &isShutdownCalled](const auto& x, const auto& y, const auto& z) {
        return shutdownDuringSecondBatch(x, y, z, doc2, &this->getExecutor(), &isShutdownCalled);
    };

    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "nextBatch" << BSON_ARRAY(doc2))
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);

    runReadyNetworkOperations();

    // Fetcher should attempt (unsuccessfully) to schedule a killCursors command.
    ASSERT_EQUALS(
        1,
        logs.countBSONContainingSubset(BSON(
            "msg" << "Failed to schedule killCursors command"
                  << "attr"
                  << BSON("error" << "ShutdownInProgress: TaskExecutor shutdown in progress"))));

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
}

TEST_F(FetcherTest, FetcherAppliesRetryStrategyToFirstCommandButNotToGetMoreRequests) {
    auto strategy = std::make_unique<DefaultRetryStrategy>(3U);

    fetcher = std::make_unique<Fetcher>(&getExecutor(),
                                        source,
                                        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                        findCmdObj,
                                        makeCallback(),
                                        rpc::makeEmptyMetadata(),
                                        executor::RemoteCommandRequest::kNoTimeout,
                                        executor::RemoteCommandRequest::kNoTimeout,
                                        std::move(strategy));

    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    // Retry strategy is applied to find command.
    const BSONObj doc = BSON("_id" << 1);
    auto rs =
        ResponseStatus::make_forTest(Status(ErrorCodes::HostUnreachable, "first"), Milliseconds(0));
    processNetworkResponse(rs, ReadyQueueState::kHasReadyRequests, FetcherState::kActive);
    rs = ResponseStatus::make_forTest(Status(ErrorCodes::SocketException, "second"),
                                      Milliseconds(0));
    processNetworkResponse(rs, ReadyQueueState::kHasReadyRequests, FetcherState::kActive);
    processNetworkResponse(BSON("cursor" << BSON("id" << 1LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSON_ARRAY(doc))
                                         << "ok" << 1),
                           ReadyQueueState::kHasReadyRequests,
                           FetcherState::kActive);
    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns_forTest());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_BSONOBJ_EQ(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);

    rs = ResponseStatus::make_forTest(Status(ErrorCodes::OperationFailed, "getMore failed"),
                                      Milliseconds(0));
    // No retry strategy for subsequent getMore commands.
    processNetworkResponse(rs, ReadyQueueState::kEmpty, FetcherState::kInactive);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
}

bool sharedCallbackStateDestroyed = false;
class SharedCallbackState {
    SharedCallbackState(const SharedCallbackState&) = delete;
    SharedCallbackState& operator=(const SharedCallbackState&) = delete;

public:
    SharedCallbackState() {}
    ~SharedCallbackState() {
        sharedCallbackStateDestroyed = true;
    }
};

TEST_F(FetcherTest, FetcherResetsInternalFinishCallbackFunctionPointerAfterLastCallback) {
    auto sharedCallbackData = std::make_shared<SharedCallbackState>();
    auto callbackInvoked = false;

    fetcher = std::make_unique<Fetcher>(
        &getExecutor(),
        source,
        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
        findCmdObj,
        [&callbackInvoked, sharedCallbackData](const StatusWith<Fetcher::QueryResponse>&,
                                               Fetcher::NextAction*,
                                               BSONObjBuilder*) { callbackInvoked = true; });

    ASSERT_OK(fetcher->schedule());

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << BSONArray())
                                         << "ok" << 1),
                           ReadyQueueState::kEmpty,
                           FetcherState::kInactive);

    ASSERT_FALSE(fetcher->isActive());

    // Fetcher should reset 'Fetcher::_work' after running callback function for the last time
    // before becoming inactive.
    // This ensures that we release resources associated with 'Fetcher::_work'.
    ASSERT_TRUE(callbackInvoked);
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

}  // namespace
