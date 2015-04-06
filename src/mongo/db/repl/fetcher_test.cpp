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

#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/fetcher.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replication_executor.h"

#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;
    using namespace mongo::repl;

    const int64_t prngSeed = 1;
    const HostAndPort target("localhost", -1);
    const BSONObj findCmdObj = BSON("find" << "coll");

    class FetcherTest : public unittest::Test {
    public:

        FetcherTest() : _net(nullptr) { }

        NetworkInterfaceMock* getNet() { return _net; }
        ReplicationExecutor& getExecutor() { return *_executor; }

        void launchExecutorThread();
        void joinExecutorThread();

        void scheduleNetworkResponse(const BSONObj& obj);
        void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason);

        virtual void setUp();
        virtual void tearDown();

    private:
        NetworkInterfaceMock* _net;
        boost::scoped_ptr<ReplicationExecutor> _executor;
        boost::scoped_ptr<boost::thread> _executorThread;
    };

    void FetcherTest::launchExecutorThread() {
        ASSERT(!_executorThread);
        _executorThread.reset(
                new boost::thread(stdx::bind(&ReplicationExecutor::run, _executor.get())));
        _net->enterNetwork();
    }

    void FetcherTest::joinExecutorThread() {
        ASSERT(_executorThread);
        _net->exitNetwork();
        _executorThread->join();
        _executorThread.reset();
    }

    void FetcherTest::scheduleNetworkResponse(const BSONObj& obj) {
        invariant(_net);
        ASSERT_TRUE(_net->hasReadyRequests());
        ReplicationExecutor::Milliseconds millis(0);
        ReplicationExecutor::RemoteCommandResponse response(obj, millis);
        ReplicationExecutor::ResponseStatus responseStatus(response);
        _net->scheduleResponse(_net->getNextReadyRequest(), _net->now(), responseStatus);
    }

    void FetcherTest::scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
        invariant(_net);
        ASSERT_TRUE(_net->hasReadyRequests());
        ReplicationExecutor::ResponseStatus responseStatus(code, reason);
        _net->scheduleResponse(_net->getNextReadyRequest(), _net->now(), responseStatus);
    }

    void FetcherTest::setUp() {
        _net = new NetworkInterfaceMock;
        _executor.reset(new ReplicationExecutor(_net, prngSeed));
    }

    void FetcherTest::tearDown() {
        if (_executorThread) {
            _executor->shutdown();
            joinExecutorThread();
        }
        _net = nullptr;
    }

    void unusedFetcherCallback(const StatusWith<Fetcher::BatchData>& fetchResult,
                               Fetcher::NextAction* nextAction) {
        FAIL("should not reach here");
    }

    TEST_F(FetcherTest, InvalidConstruction) {
        ReplicationExecutor& executor = getExecutor();

        // Null executor.
        ASSERT_THROWS(Fetcher(nullptr, target, "db", findCmdObj, unusedFetcherCallback),
                      UserException);

        // Empty database name.
        ASSERT_THROWS(Fetcher(&executor, target, "", findCmdObj, unusedFetcherCallback),
                      UserException);

        // Empty command object.
        ASSERT_THROWS(Fetcher(&executor, target, "db", BSONObj(), unusedFetcherCallback),
                      UserException);

        // First field of command object must be named "find".
        ASSERT_THROWS(Fetcher(&executor, target, "db", BSON("a" << "coll"), unusedFetcherCallback),
                      UserException);

        // First field of command object must be a string.
        ASSERT_THROWS(Fetcher(&executor, target, "db", BSON("find" << 123), unusedFetcherCallback),
                      UserException);

        // First field of command object must not be an empty string.
        ASSERT_THROWS(Fetcher(&executor, target, "db", BSON("find" << ""), unusedFetcherCallback),
                      UserException);

        // Callback function cannot be null.
        ASSERT_THROWS(Fetcher(&executor, target, "db", findCmdObj, Fetcher::CallbackFn()),
                      UserException);
    }

    TEST_F(FetcherTest, GetDiagnosticString) {
        ReplicationExecutor& executor = getExecutor();
        Fetcher fetcher(&executor, target, "db", findCmdObj, unusedFetcherCallback);
        ASSERT_FALSE(fetcher.getDiagnosticString().empty());
    }

    void isActiveCallback(const StatusWith<Fetcher::BatchData>& fetchResult,
                          Fetcher::NextAction* nextAction) { }

    TEST_F(FetcherTest, IsActiveAfterSchedule) {
        Fetcher fetcher(&getExecutor(), target, "db", findCmdObj, isActiveCallback);
        ASSERT_FALSE(fetcher.isActive());
        ASSERT_OK(fetcher.schedule());
        ASSERT_TRUE(fetcher.isActive());
    }

    TEST_F(FetcherTest, CancelWithoutSchedule) {
        Fetcher fetcher(&getExecutor(), target, "db", findCmdObj, isActiveCallback);
        ASSERT_FALSE(fetcher.isActive());
        fetcher.cancel();
    }

    TEST_F(FetcherTest, WaitWithoutSchedule) {
        Fetcher fetcher(&getExecutor(), target, "db", findCmdObj, isActiveCallback);
        ASSERT_FALSE(fetcher.isActive());
        fetcher.wait();
    }

    TEST_F(FetcherTest, ShutdownBeforeSchedule) {
        Fetcher fetcher(&getExecutor(), target, "db", findCmdObj, isActiveCallback);
        getExecutor().shutdown();
        ASSERT_NOT_OK(fetcher.schedule());
    }

    class FetcherBatchTest : public FetcherTest {
    public:
        static Status getDefaultStatus();
        FetcherBatchTest();
        virtual void setUp();
        virtual void tearDown();
        void clear();
        void processNetworkResponse(const BSONObj& obj);
        void processNetworkResponse(ErrorCodes::Error code, const std::string& reason);
        void finishProcessingNetworkResponse();
        Status status;
        CursorId cursorId;
        Fetcher::Documents documents;
        Fetcher::NextAction nextAction;
        Fetcher::NextAction newNextAction;
        boost::scoped_ptr<Fetcher> fetcher;
        // Called at end of _callback
        Fetcher::CallbackFn callbackHook;
    private:
        void _callback(const StatusWith<Fetcher::BatchData>& result,
                       Fetcher::NextAction* nextAction);
    };

    Status FetcherBatchTest::getDefaultStatus() {
        return Status(ErrorCodes::InternalError, "Not mutated");
    }

    FetcherBatchTest::FetcherBatchTest()
        : status(getDefaultStatus()),
          cursorId(-1),
          nextAction(Fetcher::NextAction::kInvalid) { }

    void FetcherBatchTest::setUp() {
        FetcherTest::setUp();
        clear();
        fetcher.reset(new Fetcher(
            &getExecutor(), target, "db", findCmdObj,
            stdx::bind(&FetcherBatchTest::_callback, this,
                       stdx::placeholders::_1, stdx::placeholders::_2)));
        launchExecutorThread();
        fetcher->schedule();
    }

    void FetcherBatchTest::tearDown() {
        FetcherTest::tearDown();
    }

    void FetcherBatchTest::clear() {
        status = getDefaultStatus();
        cursorId = -1;
        documents.clear();
        nextAction = Fetcher::NextAction::kInvalid;
    }

    void FetcherBatchTest::processNetworkResponse(const BSONObj& obj) {
        scheduleNetworkResponse(obj);
        finishProcessingNetworkResponse();
    }

    void FetcherBatchTest::processNetworkResponse(ErrorCodes::Error code,
                                                  const std::string& reason) {
        scheduleNetworkResponse(code, reason);
        finishProcessingNetworkResponse();
    }

    void FetcherBatchTest::finishProcessingNetworkResponse() {
        clear();
        ASSERT_TRUE(fetcher->isActive());
        getNet()->runReadyNetworkOperations();
        ASSERT_FALSE(getNet()->hasReadyRequests());
        fetcher->wait();
        ASSERT_FALSE(fetcher->isActive());
    }

    void FetcherBatchTest::_callback(const StatusWith<Fetcher::BatchData>& result,
                                     Fetcher::NextAction* nextActionFromFetcher) {
        status = result.getStatus();
        if (result.isOK()) {
            const Fetcher::BatchData& batchData = result.getValue();
            cursorId = batchData.cursorId;
            documents = batchData.documents;
        }
        nextAction = *nextActionFromFetcher;
        if (callbackHook) {
            callbackHook(result, nextActionFromFetcher);
        }
    }

    TEST_F(FetcherBatchTest, ScheduleAndCancel) {
        scheduleNetworkResponse(BSON("ok" << 1));

        fetcher->cancel();
        finishProcessingNetworkResponse();

        ASSERT_EQUALS(getDefaultStatus(), status);
    }

    TEST_F(FetcherBatchTest, ScheduleButShutdown) {
        scheduleNetworkResponse(BSON("ok" << 1));

        getExecutor().shutdown();
        // Network interface should not deliver mock response to callback.
        finishProcessingNetworkResponse();

        ASSERT_EQUALS(getDefaultStatus(), status);
    }

    TEST_F(FetcherBatchTest, FindCommandFailed1) {
        processNetworkResponse(ErrorCodes::BadValue, "bad hint");
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_EQUALS("bad hint", status.reason());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, FindCommandFailed2) {
        processNetworkResponse(BSON("ok" << 0 <<
                                    "errmsg" << "bad hint" <<
                                    "code" << int(ErrorCodes::BadValue)));
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_EQUALS("bad hint", status.reason());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, CursorFieldMissing) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor' field");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, CursorNotAnObject) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << 123 << "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor' field must be an object");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, CursorIdFieldMissing) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("ns" << "db.coll" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.id' field");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, CursorIdNotLongNumber) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 123 <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(),
                               "'cursor.id' field must be a number of type 'long'");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, NamespaceFieldMissing) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.ns' field");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, NamespaceNotAString) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "ns" << 123 <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' field must be a string");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, NamespaceEmpty) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "ns" << "" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, NamespaceMissingCollectionName) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "ns" << "db." <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, FirstBatchFieldMissing) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll") <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.firstBatch' field");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, FirstBatchNotAnArray) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << 123) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.firstBatch' field must be an array");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, FirstBatchArrayContainsNonObject) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSON_ARRAY(8)) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "found non-object");
        ASSERT_STRING_CONTAINS(status.reason(), "in 'cursor.firstBatch' field");
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, FirstBatchEmptyArray) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_OK(status);
        ASSERT_TRUE(documents.empty());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, FetchOneDocument) {
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSON_ARRAY(doc)) <<
                                    "ok" << 1));
        ASSERT_OK(status);
        ASSERT_EQUALS(0, cursorId);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
    }

    TEST_F(FetcherBatchTest, FetchMultipleBatches) {
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc3 = BSON("_id" << 3);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc3)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc3, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
        ASSERT_FALSE(fetcher->isActive());

        ASSERT_FALSE(getNet()->hasReadyRequests());
        ASSERT_FALSE(fetcher->isActive());
    }

    TEST_F(FetcherBatchTest, ScheduleGetMoreAndCancel) {
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        fetcher->cancel();
        finishProcessingNetworkResponse();
        ASSERT_NOT_OK(status);
    }

    TEST_F(FetcherBatchTest, ScheduleGetMoreButShutdown) {
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        getExecutor().shutdown();
        finishProcessingNetworkResponse();
        ASSERT_NOT_OK(status);
    }

    void setNextActionToNoAction(const StatusWith<Fetcher::BatchData>& fetchResult,
                                 Fetcher::NextAction* nextAction) {
        *nextAction = Fetcher::NextAction::kNoAction;
    }

    TEST_F(FetcherBatchTest, UpdateNextActionAfterSecondBatch) {

        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));

        callbackHook = setNextActionToNoAction;

        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_FALSE(fetcher->isActive());
    }

    /**
     * This will be invoked twice before the fetcher returns control to the replication executor.
     */
    void shutdownDuringSecondBatch(const StatusWith<Fetcher::BatchData>& fetchResult,
                                   Fetcher::NextAction* nextAction,
                                   const BSONObj& doc2,
                                   ReplicationExecutor* executor, bool* isShutdownCalled) {
        if (*isShutdownCalled) {
            return;
        }

        // First time during second batch
        ASSERT_OK(fetchResult.getStatus());
        ASSERT_EQUALS(1U, fetchResult.getValue().documents.size());
        ASSERT_EQUALS(doc2, fetchResult.getValue().documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == *nextAction);

        executor->shutdown();
        *isShutdownCalled = true;
    }

    TEST_F(FetcherBatchTest, ShutdownDuringSecondBatch) {
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kContinue == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));

        bool isShutdownCalled = false;
        callbackHook = stdx::bind(shutdownDuringSecondBatch,
                                  stdx::placeholders::_1, stdx::placeholders::_2,
                                  doc2,
                                  &getExecutor(), &isShutdownCalled);

        getNet()->runReadyNetworkOperations();
        ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
        ASSERT_FALSE(fetcher->isActive());
    }

} // namespace
