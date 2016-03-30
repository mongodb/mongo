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

#include "mongo/client/fetcher.h"
#include "mongo/db/jsobj.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/rpc/metadata.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using executor::NetworkInterfaceMock;
using executor::TaskExecutor;

const HostAndPort source("localhost", -1);
const BSONObj findCmdObj = BSON("find"
                                << "coll");

class FetcherTest : public executor::ThreadPoolExecutorTest {
public:
    FetcherTest();
    void clear();
    void scheduleNetworkResponse(const BSONObj& obj);
    void scheduleNetworkResponse(const BSONObj& obj, Milliseconds millis);
    void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason);
    void scheduleNetworkResponseFor(const BSONObj& filter, const BSONObj& obj);
    void scheduleNetworkResponseFor(NetworkInterfaceMock::NetworkOperationIterator noi,
                                    const BSONObj& obj);

    // Calls scheduleNetworkResponse + finishProcessingNetworkResponse
    void processNetworkResponse(const BSONObj& obj);
    // Calls scheduleNetworkResponse + finishProcessingNetworkResponse
    void processNetworkResponse(ErrorCodes::Error code, const std::string& reason);

    void finishProcessingNetworkResponse();

protected:
    void setUp() override;
    void tearDown() override;

    Status status;
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
    void _callback(const StatusWith<Fetcher::QueryResponse>& result,
                   Fetcher::NextAction* nextAction,
                   BSONObjBuilder* getMoreBob);
};

FetcherTest::FetcherTest()
    : status(getDetectableErrorStatus()), cursorId(-1), nextAction(Fetcher::NextAction::kInvalid) {}

void FetcherTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    clear();
    fetcher = stdx::make_unique<Fetcher>(&getExecutor(),
                                         source,
                                         "db",
                                         findCmdObj,
                                         stdx::bind(&FetcherTest::_callback,
                                                    this,
                                                    stdx::placeholders::_1,
                                                    stdx::placeholders::_2,
                                                    stdx::placeholders::_3));
    launchExecutorThread();
}

void FetcherTest::tearDown() {
    executor::ThreadPoolExecutorTest::tearDown();
    // Executor may still invoke fetcher's callback before shutting down.
    fetcher.reset();
}

void FetcherTest::clear() {
    status = getDetectableErrorStatus();
    cursorId = -1;
    nss = NamespaceString();
    documents.clear();
    elapsedMillis = Milliseconds(0);
    first = false;
    nextAction = Fetcher::NextAction::kInvalid;
    callbackHook = Fetcher::CallbackFn();
}

void FetcherTest::scheduleNetworkResponse(const BSONObj& obj) {
    scheduleNetworkResponse(obj, Milliseconds(0));
}

void FetcherTest::scheduleNetworkResponse(const BSONObj& obj, Milliseconds millis) {
    NetworkInterfaceMock* net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    executor::RemoteCommandResponse response(obj, BSONObj(), millis);
    TaskExecutor::ResponseStatus responseStatus(response);
    net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
}
void FetcherTest::scheduleNetworkResponseFor(const BSONObj& filter, const BSONObj& obj) {
    ASSERT_TRUE(filter[1].eoo());  // The filter should only have one field, to match the cmd name
    NetworkInterfaceMock* net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    Milliseconds millis(0);
    executor::RemoteCommandResponse response(obj, BSONObj(), millis);
    TaskExecutor::ResponseStatus responseStatus(response);
    auto req = net->getNextReadyRequest();
    ASSERT_EQ(req->getRequest().cmdObj[0], filter[0]);
    net->scheduleResponse(req, net->now(), responseStatus);
}

void FetcherTest::scheduleNetworkResponseFor(NetworkInterfaceMock::NetworkOperationIterator noi,
                                             const BSONObj& obj) {
    NetworkInterfaceMock* net = getNet();
    Milliseconds millis(0);
    executor::RemoteCommandResponse response(obj, BSONObj(), millis);
    TaskExecutor::ResponseStatus responseStatus(response);
    net->scheduleResponse(noi, net->now(), responseStatus);
}

void FetcherTest::scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
    NetworkInterfaceMock* net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    TaskExecutor::ResponseStatus responseStatus(code, reason);
    net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
}

void FetcherTest::processNetworkResponse(const BSONObj& obj) {
    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(obj);
    finishProcessingNetworkResponse();
    net->exitNetwork();
}

void FetcherTest::processNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(code, reason);
    finishProcessingNetworkResponse();
    net->exitNetwork();
}

void FetcherTest::finishProcessingNetworkResponse() {
    clear();
    ASSERT_TRUE(fetcher->isActive());
    getNet()->runReadyNetworkOperations();
    ASSERT_FALSE(getNet()->hasReadyRequests());
    ASSERT_FALSE(fetcher->isActive());
}

void FetcherTest::_callback(const StatusWith<Fetcher::QueryResponse>& result,
                            Fetcher::NextAction* nextActionFromFetcher,
                            BSONObjBuilder* getMoreBob) {
    status = result.getStatus();
    if (result.isOK()) {
        const Fetcher::QueryResponse& batchData = result.getValue();
        cursorId = batchData.cursorId;
        nss = batchData.nss;
        documents = batchData.documents;
        elapsedMillis = batchData.elapsedMillis;
        first = batchData.first;
    }

    if (callbackHook) {
        callbackHook(result, nextActionFromFetcher, getMoreBob);
    }

    if (nextActionFromFetcher) {
        nextAction = *nextActionFromFetcher;
    }
}

void unreachableCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                         Fetcher::NextAction* nextAction,
                         BSONObjBuilder* getMoreBob) {
    FAIL("should not reach here");
}

void doNothingCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                       Fetcher::NextAction* nextAction,
                       BSONObjBuilder* getMoreBob) {}

TEST_F(FetcherTest, InvalidConstruction) {
    TaskExecutor& executor = getExecutor();

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(nullptr, source, "db", findCmdObj, unreachableCallback),
                                UserException,
                                ErrorCodes::BadValue,
                                "null task executor");

    // Empty database name.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(&executor, source, "", findCmdObj, unreachableCallback),
                                UserException,
                                ErrorCodes::BadValue,
                                "database name cannot be empty");

    // Empty command object.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(&executor, source, "db", BSONObj(), unreachableCallback),
                                UserException,
                                ErrorCodes::BadValue,
                                "command object cannot be empty");

    // Callback function cannot be null.
    ASSERT_THROWS_CODE_AND_WHAT(Fetcher(&executor, source, "db", findCmdObj, Fetcher::CallbackFn()),
                                UserException,
                                ErrorCodes::BadValue,
                                "callback function cannot be null");
}

// Command object can refer to any command that returns a cursor. This
// includes listIndexes and listCollections.
TEST_F(FetcherTest, NonFindCommand) {
    TaskExecutor& executor = getExecutor();

    Fetcher(&executor,
            source,
            "db",
            BSON("listIndexes"
                 << "coll"),
            unreachableCallback);
    Fetcher(&executor, source, "db", BSON("listCollections" << 1), unreachableCallback);
    Fetcher(&executor, source, "db", BSON("a" << 1), unreachableCallback);
}

TEST_F(FetcherTest, RemoteCommandRequestShouldContainCommandParametersPassedToConstructor) {
    auto metadataObj = BSON("x" << 1);
    Milliseconds timeout(8000);

    fetcher = stdx::make_unique<Fetcher>(
        &getExecutor(), source, "db", findCmdObj, doNothingCallback, metadataObj, timeout);

    ASSERT_EQUALS(source, fetcher->getSource());
    ASSERT_EQUALS(findCmdObj, fetcher->getCommandObject());
    ASSERT_EQUALS(metadataObj, fetcher->getMetadataObject());
    ASSERT_EQUALS(timeout, fetcher->getTimeout());

    ASSERT_OK(fetcher->schedule());

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto request = noi->getRequest();
    net->exitNetwork();

    ASSERT_EQUALS(source, request.target);
    ASSERT_EQUALS(findCmdObj, request.cmdObj);
    ASSERT_EQUALS(metadataObj, request.metadata);
    ASSERT_EQUALS(timeout, request.timeout);
}

TEST_F(FetcherTest, GetDiagnosticString) {
    ASSERT_FALSE(fetcher->getDiagnosticString().empty());
}

TEST_F(FetcherTest, IsActiveAfterSchedule) {
    ASSERT_FALSE(fetcher->isActive());
    ASSERT_OK(fetcher->schedule());
    ASSERT_TRUE(fetcher->isActive());
}

TEST_F(FetcherTest, ScheduleWhenActive) {
    ASSERT_OK(fetcher->schedule());
    ASSERT_TRUE(fetcher->isActive());
    ASSERT_NOT_OK(fetcher->schedule());
}

TEST_F(FetcherTest, CancelWithoutSchedule) {
    ASSERT_FALSE(fetcher->isActive());
    fetcher->cancel();
}

TEST_F(FetcherTest, WaitWithoutSchedule) {
    ASSERT_FALSE(fetcher->isActive());
    fetcher->wait();
}

TEST_F(FetcherTest, ShutdownBeforeSchedule) {
    getExecutor().shutdown();
    ASSERT_NOT_OK(fetcher->schedule());
    ASSERT_FALSE(fetcher->isActive());
}

TEST_F(FetcherTest, ScheduleAndCancel) {
    ASSERT_OK(fetcher->schedule());

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(BSON("ok" << 1));
    net->exitNetwork();

    fetcher->cancel();

    net->enterNetwork();
    finishProcessingNetworkResponse();
    net->exitNetwork();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status.code());
}

TEST_F(FetcherTest, ScheduleButShutdown) {
    ASSERT_OK(fetcher->schedule());

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(BSON("ok" << 1));
    // Network interface should not deliver mock response to callback
    // until runReadyNetworkOperations() is called.
    net->exitNetwork();

    ASSERT_TRUE(fetcher->isActive());
    getExecutor().shutdown();

    net->enterNetwork();
    net->runReadyNetworkOperations();
    net->exitNetwork();
    ASSERT_FALSE(fetcher->isActive());

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status.code());
}

TEST_F(FetcherTest, FindCommandFailed1) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(ErrorCodes::BadValue, "bad hint");
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_EQUALS("bad hint", status.reason());
}

TEST_F(FetcherTest, FindCommandFailed2) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                     << "bad hint"
                                     << "code" << int(ErrorCodes::BadValue)));
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_EQUALS("bad hint", status.reason());
}

TEST_F(FetcherTest, CursorFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor' field");
}

TEST_F(FetcherTest, CursorNotAnObject) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << 123 << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor' field must be an object");
}

TEST_F(FetcherTest, CursorIdFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("ns"
                                                 << "db.coll"
                                                 << "firstBatch" << BSONArray()) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.id' field");
}

TEST_F(FetcherTest, CursorIdNotLongNumber) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 123.1 << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSONArray()) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.id' field must be");
    ASSERT_EQ((int)Fetcher::NextAction::kInvalid, (int)nextAction);
}

TEST_F(FetcherTest, NamespaceFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 123LL << "firstBatch" << BSONArray()) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.ns' field");
}

TEST_F(FetcherTest, NamespaceNotAString) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 123LL << "ns" << 123 << "firstBatch"
                                                      << BSONArray()) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' field must be a string");
}

TEST_F(FetcherTest, NamespaceEmpty) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 123LL << "ns"
                                   << ""
                                   << "firstBatch" << BSONArray()) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
}

TEST_F(FetcherTest, NamespaceMissingCollectionName) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 123LL << "ns"
                                   << "db."
                                   << "firstBatch" << BSONArray()) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
}

TEST_F(FetcherTest, FirstBatchFieldMissing) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll") << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.firstBatch' field");
}

TEST_F(FetcherTest, FirstBatchNotAnArray) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(BSON("cursor" << BSON("id" << 0LL << "ns"
                                                      << "db.coll"
                                                      << "firstBatch" << 123) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "'cursor.firstBatch' field must be an array");
}

TEST_F(FetcherTest, FirstBatchArrayContainsNonObject) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 0LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(8)) << "ok" << 1));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "found non-object");
    ASSERT_STRING_CONTAINS(status.reason(), "in 'cursor.firstBatch' field");
}

TEST_F(FetcherTest, FirstBatchEmptyArray) {
    ASSERT_OK(fetcher->schedule());
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 0LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSONArray()) << "ok" << 1));
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_TRUE(documents.empty());
}

TEST_F(FetcherTest, FetchOneDocument) {
    ASSERT_OK(fetcher->schedule());
    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 0LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1));
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
}

TEST_F(FetcherTest, SetNextActionToContinueWhenNextBatchIsNotAvailable) {
    ASSERT_OK(fetcher->schedule());
    const BSONObj doc = BSON("_id" << 1);
    callbackHook = [](const StatusWith<Fetcher::QueryResponse>& fetchResult,
                      Fetcher::NextAction* nextAction,
                      BSONObjBuilder* getMoreBob) {
        ASSERT_OK(fetchResult.getStatus());
        Fetcher::QueryResponse batchData{fetchResult.getValue()};

        ASSERT(nextAction);
        *nextAction = Fetcher::NextAction::kGetMore;
        ASSERT_FALSE(getMoreBob);
    };
    processNetworkResponse(
        BSON("cursor" << BSON("id" << 0LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1));
    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
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

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1),
        Milliseconds(100));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
    ASSERT_EQUALS(elapsedMillis, Milliseconds(100));
    ASSERT_TRUE(first);
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    const BSONObj doc2 = BSON("_id" << 2);

    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSON_ARRAY(doc2)) << "ok" << 1),
        Milliseconds(200));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc2, documents.front());
    ASSERT_EQUALS(elapsedMillis, Milliseconds(200));
    ASSERT_FALSE(first);
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    const BSONObj doc3 = BSON("_id" << 3);

    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 0LL << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSON_ARRAY(doc3)) << "ok" << 1),
        Milliseconds(300));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(0, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc3, documents.front());
    ASSERT_EQUALS(elapsedMillis, Milliseconds(300));
    ASSERT_FALSE(first);
    ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    ASSERT_FALSE(fetcher->isActive());

    net->enterNetwork();
    ASSERT_FALSE(getNet()->hasReadyRequests());
    net->exitNetwork();
}

TEST_F(FetcherTest, ScheduleGetMoreAndCancel) {
    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    const BSONObj doc2 = BSON("_id" << 2);
    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSON_ARRAY(doc2)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc2, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    fetcher->cancel();

    net->enterNetwork();
    finishProcessingNetworkResponse();
    net->exitNetwork();

    ASSERT_NOT_OK(status);
}

TEST_F(FetcherTest, ScheduleGetMoreButShutdown) {
    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    const BSONObj doc2 = BSON("_id" << 2);

    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSON_ARRAY(doc2)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc2, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    getExecutor().shutdown();

    net->enterNetwork();
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_NOT_OK(status);
}


TEST_F(FetcherTest, EmptyGetMoreRequestAfterFirstBatchMakesFetcherInactiveAndKillsCursor) {
    startCapturingLogMessages();

    callbackHook = [](const StatusWith<Fetcher::QueryResponse>& fetchResult,
                      Fetcher::NextAction* nextAction,
                      BSONObjBuilder* getMoreBob) {};

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_FALSE(fetcher->isActive());

    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    auto noi = getNet()->getNextReadyRequest();
    auto request = noi->getRequest();
    net->exitNetwork();

    ASSERT_EQUALS(nss.db(), request.dbname);
    auto&& cmdObj = request.cmdObj;
    auto firstElement = cmdObj.firstElement();
    ASSERT_EQUALS("killCursors", firstElement.fieldNameStringData());
    ASSERT_EQUALS(nss.coll(), firstElement.String());
    ASSERT_EQUALS(mongo::BSONType::Array, cmdObj["cursors"].type());
    auto cursors = cmdObj["cursors"].Array();
    ASSERT_EQUALS(1U, cursors.size());
    ASSERT_EQUALS(cursorId, cursors.front().numberLong());

    // killCursors command request will be canceled by executor on shutdown.
    tearDown();
    ASSERT_EQUALS(1,
                  countLogLinesContaining(
                      "killCursors command task failed: CallbackCanceled: Callback canceled"));
}

void setNextActionToNoAction(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                             Fetcher::NextAction* nextAction,
                             BSONObjBuilder* getMoreBob) {
    *nextAction = Fetcher::NextAction::kNoAction;
}

TEST_F(FetcherTest, UpdateNextActionAfterSecondBatch) {
    startCapturingLogMessages();

    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    const BSONObj doc2 = BSON("_id" << 2);

    callbackHook = setNextActionToNoAction;

    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSON_ARRAY(doc2)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc2, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    ASSERT_FALSE(fetcher->isActive());

    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    auto noi = getNet()->getNextReadyRequest();
    auto request = noi->getRequest();
    net->exitNetwork();

    ASSERT_EQUALS(nss.db(), request.dbname);
    auto&& cmdObj = request.cmdObj;
    auto firstElement = cmdObj.firstElement();
    ASSERT_EQUALS("killCursors", firstElement.fieldNameStringData());
    ASSERT_EQUALS(nss.coll(), firstElement.String());
    ASSERT_EQUALS(mongo::BSONType::Array, cmdObj["cursors"].type());
    auto cursors = cmdObj["cursors"].Array();
    ASSERT_EQUALS(1U, cursors.size());
    ASSERT_EQUALS(cursorId, cursors.front().numberLong());

    // Failed killCursors command response should be logged.
    net->enterNetwork();
    scheduleNetworkResponseFor(noi, BSON("ok" << false));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_EQUALS(1, countLogLinesContaining("killCursors command failed: UnknownError"));
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
    Fetcher::QueryResponse batchData{fetchResult.getValue()};
    ASSERT_EQUALS(1U, batchData.documents.size());
    ASSERT_EQUALS(doc2, batchData.documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == *nextAction);
    ASSERT(getMoreBob);
    getMoreBob->append("getMore", batchData.cursorId);
    getMoreBob->append("collection", batchData.nss.coll());

    executor->shutdown();
    *isShutdownCalled = true;
}

TEST_F(FetcherTest, ShutdownDuringSecondBatch) {
    startCapturingLogMessages();

    callbackHook = appendGetMoreRequest;

    ASSERT_OK(fetcher->schedule());

    const BSONObj doc = BSON("_id" << 1);

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON_ARRAY(doc)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(status);
    ASSERT_EQUALS(1LL, cursorId);
    ASSERT_EQUALS("db.coll", nss.ns());
    ASSERT_EQUALS(1U, documents.size());
    ASSERT_EQUALS(doc, documents.front());
    ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
    ASSERT_TRUE(fetcher->isActive());

    const BSONObj doc2 = BSON("_id" << 2);

    bool isShutdownCalled = false;
    callbackHook = stdx::bind(shutdownDuringSecondBatch,
                              stdx::placeholders::_1,
                              stdx::placeholders::_2,
                              stdx::placeholders::_3,
                              doc2,
                              &getExecutor(),
                              &isShutdownCalled);

    net->enterNetwork();
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(
        BSON("cursor" << BSON("id" << 1LL << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSON_ARRAY(doc2)) << "ok" << 1));
    getNet()->runReadyNetworkOperations();
    net->exitNetwork();

    // Fetcher should attempt (unsuccessfully) to schedule a killCursors command.
    ASSERT_EQUALS(
        1,
        countLogLinesContaining(
            "failed to schedule killCursors command: ShutdownInProgress: Shutdown in progress"));

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(fetcher->isActive());
}

}  // namespace
