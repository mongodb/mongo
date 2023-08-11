/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include <boost/smart_ptr.hpp>
#include <climits>
#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/pinned_connection_task_executor_test_fixture.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {
namespace {

BSONObj buildCursorResponse(StringData fieldName, size_t start, size_t end, size_t cursorId) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder cursor(bob.subobjStart("cursor"));
        {
            BSONArrayBuilder batch(cursor.subarrayStart(fieldName));

            for (size_t i = start; i <= end; ++i) {
                BSONObjBuilder doc(batch.subobjStart());
                doc.append("x", int(i));
            }
        }
        cursor.append("id", (long long)(cursorId));
        cursor.append("ns", "test.test");
    }
    bob.append("ok", int(1));
    return bob.obj();
}

BSONObj buildMultiCursorResponse(StringData fieldName,
                                 size_t start,
                                 size_t end,
                                 std::vector<size_t> cursorIds) {
    BSONObjBuilder bob;
    {
        BSONArrayBuilder cursors;
        int baseCursorValue = 1;
        for (auto cursorId : cursorIds) {
            BSONObjBuilder cursor;
            BSONArrayBuilder batch;
            ASSERT(start < end && end < INT_MAX);
            for (size_t i = start; i <= end; ++i) {
                batch.append(BSON("x" << static_cast<int>(i) * baseCursorValue).getOwned());
            }
            cursor.append(fieldName, batch.arr());
            cursor.append("id", (long long)(cursorId));
            cursor.append("ns", "test.test");
            auto cursorObj = BSON("cursor" << cursor.done() << "ok" << 1);
            cursors.append(cursorObj.getOwned());
            ++baseCursorValue;
        }
        bob.append("cursors", cursors.arr());
    }
    bob.append("ok", 1);
    return bob.obj();
}

/**
 * Fixture for the task executor cursor tests which offers some convenience methods to help with
 * scheduling responses. Uses the CRTP pattern so that the tests can be shared between child-classes
 * that provide their own implementations of the network-mocking needed for the tests.
 */
template <typename Derived, typename Base>
class TaskExecutorCursorTestFixture : public Base {
public:
    void setUp() override {
        Base::setUp();
        client = serviceCtx->makeClient("TaskExecutorCursorTest");
        opCtx = client->makeOperationContext();
        static_cast<Derived*>(this)->postSetUp();
    }

    void tearDown() override {
        opCtx.reset();
        client.reset();

        Base::tearDown();
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId) {
        return static_cast<Derived*>(this)->scheduleSuccessfulCursorResponse(
            fieldName, start, end, cursorId);
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds) {
        return static_cast<Derived*>(this)->scheduleSuccessfulMultiCursorResponse(
            fieldName, start, end, cursorIds);
    }

    void scheduleErrorResponse(Status error) {
        return static_cast<Derived*>(this)->scheduleErrorResponse(error);
    }
    void blackHoleNextOutgoingRequest() {
        return static_cast<Derived*>(this)->blackHoleNextOutgoingRequest();
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId) {
        return static_cast<Derived*>(this)->scheduleSuccessfulKillCursorResponse(cursorId);
    }

    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        return static_cast<Derived*>(this)->makeTec(rcr, std::move(options));
    }

    bool hasReadyRequests() {
        return static_cast<Derived*>(this)->hasReadyRequests();
    }

    Base& asBase() {
        return *this;
    }

    /**
     * Ensure we work for a single simple batch
     */
    void SingleBatchWorksTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        const CursorId cursorId = 0;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_FALSE(hasReadyRequests());

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec.getNext(opCtx.get()));
    }

    /**
     * Ensure the firstBatch can be read correctly when multiple cursors are returned.
     */
    void MultipleCursorsSingleBatchSucceedsTest() {
        const auto aggCmd = BSON("aggregate"
                                 << "test"
                                 << "pipeline"
                                 << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 aggCmd,
                                 opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(aggCmd,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, {0, 0}));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec.getNext(opCtx.get()));

        auto cursorVec = tec.releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
    }
    /**
     * The operation context under which we send the original cursor-establishing command
     * can be destructed before getNext is called with new opCtx. Ensure that 'child'
     * TaskExecutorCursors created from the original TEC's multi-cursor-response can safely
     * operate if this happens/don't try and use the now-destroyed operation context.
     * See SERVER-69702 for context
     */
    void ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest() {
        auto lsid = makeLogicalSessionIdForTest();
        opCtx->setLogicalSessionId(lsid);
        const auto aggCmd = BSON("aggregate"
                                 << "test"
                                 << "pipeline"
                                 << BSON_ARRAY(BSON("returnMultipleCursors" << true)));
        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 aggCmd,
                                 opCtx.get());
        TaskExecutorCursor tec = makeTec(rcr);
        auto expected = BSON("aggregate"
                             << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true))
                             << "lsid" << lsid.toBSON());
        ASSERT_BSONOBJ_EQ(expected,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, {0, 0}));
        // Before calling getNext (and therefore spawning child TECs), destroy the opCtx
        // we used to send the initial query and make a new one.
        opCtx.reset();
        opCtx = client->makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        // Use the new opCtx to call getNext. The child TECs should not attempt to read from the
        // now dead original opCtx.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec.getNext(opCtx.get()));

        auto cursorVec = tec.releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
    }

    void MultipleCursorsGetMoreWorksTest() {
        const auto aggCmd = BSON("aggregate"
                                 << "test"
                                 << "pipeline"
                                 << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

        std::vector<size_t> cursorIds{1, 2};
        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 aggCmd,
                                 opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(aggCmd,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, cursorIds));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        auto cursorVec = tec.releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);

        // If we try to getNext() at this point, we are interruptible and can timeout
        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { tec.getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        // We can pick up after that interruption though
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 3, 5, cursorIds[0]));

        // Repeat for second cursor.
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 4);

        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { secondCursor.getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        ASSERT_BSONOBJ_EQ(BSON("getMore" << 2LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 6, 8, cursorIds[1]));
        // Read second batch, then schedule EOF on both cursors.
        // Then read final document for each.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 3);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 5);
        scheduleSuccessfulCursorResponse("nextBatch", 6, 6, 0);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 6);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 6);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 7);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 8);
        scheduleSuccessfulCursorResponse("nextBatch", 12, 12, 0);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 12);

        // Shouldn't have any more requests, both cursors are closed.
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(tec.getNext(opCtx.get()));
        ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
    }

    /**
     * Ensure we work if find fails (and that we receive the error code it failed with)
     */
    void FailureInFindTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        scheduleErrorResponse(Status(ErrorCodes::BadValue, "an error"));

        ASSERT_THROWS_CODE(tec.getNext(opCtx.get()), DBException, ErrorCodes::BadValue);
    }


    /**
     * Ensure multiple batches works correctly
     */
    void MultipleBatchesWorksTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr, [] {
            TaskExecutorCursor::Options opts;
            opts.batchSize = 3;
            return opts;
        }());

        scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        // ASSERT(hasReadyRequests());

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        // If we try to getNext() at this point, we are interruptible and can timeout
        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { tec.getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        // We can pick up after that interruption though
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                         << "test"
                                         << "batchSize" << 3),
                          scheduleSuccessfulCursorResponse("nextBatch", 3, 5, cursorId));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 3);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 5);

        cursorId = 0;
        scheduleSuccessfulCursorResponse("nextBatch", 6, 6, cursorId);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 6);

        // We don't issue extra getmores after returning a 0 cursor id
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(tec.getNext(opCtx.get()));
    }

    /**
     * Ensure we allow empty firstBatch.
     */
    void EmptyFirstBatchTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                               << "test"
                                               << "batchSize" << 3);
        const CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr, [] {
            TaskExecutorCursor::Options opts;
            opts.batchSize = 3;
            return opts;
        }());

        // Schedule a cursor response with an empty "firstBatch". Use end < start so we don't
        // append any doc to "firstBatch".
        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 0, cursorId));

        stdx::thread th([&] {
            // Wait for the getMore run by the getNext() below to be ready, and schedule a
            // cursor response with a non-empty "nextBatch".
            while (!hasReadyRequests()) {
                sleepmillis(10);
            }

            ASSERT_BSONOBJ_EQ(getMoreCmd, scheduleSuccessfulCursorResponse("nextBatch", 1, 1, 0));
        });

        // Verify that the first doc is the doc from the second batch.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        th.join();
    }

    /**
     * Ensure we allow any empty non-initial batch.
     */
    void EmptyNonInitialBatchTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                               << "test"
                                               << "batchSize" << 3);
        const CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr, [] {
            TaskExecutorCursor::Options opts;
            opts.batchSize = 3;
            return opts;
        }());

        // Schedule a cursor response with a non-empty "firstBatch".
        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 1, cursorId));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        // Schedule two consecutive cursor responses with empty "nextBatch". Use end < start so
        // we don't append any doc to "nextBatch".
        ASSERT_BSONOBJ_EQ(getMoreCmd,
                          scheduleSuccessfulCursorResponse("nextBatch", 1, 0, cursorId));

        stdx::thread th([&] {
            // Wait for the first getMore run by the getNext() below to be ready, and schedule a
            // cursor response with a non-empty "nextBatch".
            while (!hasReadyRequests()) {
                sleepmillis(10);
            }

            ASSERT_BSONOBJ_EQ(getMoreCmd,
                              scheduleSuccessfulCursorResponse("nextBatch", 1, 0, cursorId));

            // Wait for the second getMore run by the getNext() below to be ready, and schedule a
            // cursor response with a non-empty "nextBatch".
            while (!hasReadyRequests()) {
                sleepmillis(10);
            }

            ASSERT_BSONOBJ_EQ(getMoreCmd, scheduleSuccessfulCursorResponse("nextBatch", 2, 2, 0));
        });

        // Verify that the next doc is the doc from the fourth batch.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        th.join();
    }

    /**
     * Test that if 'preFetchNextBatch' is false, the TaskExecutorCursor does not request GetMores
     * until the current batch is exhausted and 'getNext()' is invoked.
     */
    void NoPrefetchGetMore() {
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search"
                                          << "foo"),
                                     opCtx.get());

            // The lambda that will be used to augment the getMore request sent below is passed into
            // the TEC constructor.
            auto augmentGetMore = [](BSONObjBuilder& bob) {
                bob.append("test", 1);
            };

            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            TaskExecutorCursor tec = makeTec(rcr, [&augmentGetMore] {
                TaskExecutorCursor::Options opts;
                opts.batchSize = 2;
                opts.preFetchNextBatch = false;
                opts.getMoreAugmentationWriter = augmentGetMore;
                return opts;
            }());

            // Mock the response for the first batch.
            scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

            // Exhaust the first batch.
            ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);
            ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

            // Assert that the TaskExecutorCursor has not requested a GetMore. This enforces that
            // 'preFetchNextBatch' works as expected.
            ASSERT_FALSE(hasReadyRequests());

            // As soon as 'getNext()' is invoked, the TaskExecutorCursor will try to send a GetMore
            // and that will block this thread in the NetworkInterfaceMock until there is a
            // scheduled response. However, we cannot schedule the cursor response on the main
            // thread before we call 'getNext()' as that will cause the NetworkInterfaceMock to
            // block until there is request enqueued ('getNext()' is the function which will enqueue
            // such as request). To avoid this deadlock, we start a new thread which will schedule a
            // response on the NetworkInterfaceMock.
            auto responseSchedulerThread = monitor.spawn([&] {
                auto recievedGetMoreCmd = scheduleSuccessfulCursorResponse("nextBatch", 3, 4, 0);

                // Assert that the command processed for the above response matches with the
                // lambda to augment the getMore command used during construction of the TEC
                // above.
                const auto expectedGetMoreCmd = BSON("getMore" << 1LL << "collection"
                                                               << "test"
                                                               << "batchSize" << 2 << "test" << 1);
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, recievedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 3);
            ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 4);

            // Joining the thread which schedules the cursor response for the GetMore here forces
            // the destructor of NetworkInterfaceMock::InNetworkGuard to run, which ensures that the
            // 'NetworkInterfaceMock' stops executing as the network thread. This is required before
            // we invoke 'hasReadyRequests()' which enters the network again.
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    ServiceContext::UniqueServiceContext serviceCtx = ServiceContext::make();
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

class NonPinningTaskExecutorCursorTestFixture
    : public TaskExecutorCursorTestFixture<NonPinningTaskExecutorCursorTestFixture,
                                           ThreadPoolExecutorTest> {
public:
    void postSetUp() {
        launchExecutorThread();
    }

    virtual BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                                     size_t start,
                                                     size_t end,
                                                     size_t cursorId) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());


        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(
            buildCursorResponse(fieldName, start, end, cursorId));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());


        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(
            buildMultiCursorResponse(fieldName, start, end, cursorIds));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(
            BSON("cursorsKilled" << BSON_ARRAY((long long)(cursorId)) << "cursorsNotFound"
                                 << BSONArray() << "cursorsAlive" << BSONArray() << "cursorsUnknown"
                                 << BSONArray() << "ok" << 1));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    void scheduleErrorResponse(Status error) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        ASSERT(getNet()->hasReadyRequests());
        getNet()->scheduleErrorResponse(error);
        getNet()->runReadyNetworkOperations();
    }

    bool hasReadyRequests() {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        return getNet()->hasReadyRequests();
    }

    void blackHoleNextOutgoingRequest() {
        NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->blackHole(getNet()->getFrontOfUnscheduledQueue());
    }

    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        options.pinConnection = false;
        return TaskExecutorCursor(getExecutorPtr(), rcr, std::move(options));
    }
};

class PinnedConnTaskExecutorCursorTestFixture
    : public TaskExecutorCursorTestFixture<PinnedConnTaskExecutorCursorTestFixture,
                                           PinnedConnectionTaskExecutorTest> {
public:
    void postSetUp() {}

    BSONObj scheduleResponse(StatusWith<BSONObj> response) {
        int32_t responseToId;
        BSONObj cmdObjReceived;
        auto pf = makePromiseFuture<void>();
        expectSinkMessage([&](Message m) {
            responseToId = m.header().getId();
            auto opMsg = OpMsgRequest::parse(m);
            cmdObjReceived = opMsg.body.removeField("$db").getOwned();
            pf.promise.emplaceValue();
            return Status::OK();
        });
        // Wait until we recieved the command request.
        pf.future.get();

        // Now we expect source message to be called and provide the response
        expectSourceMessage([=]() {
            rpc::OpMsgReplyBuilder replyBuilder;
            replyBuilder.setCommandReply(response);
            auto message = replyBuilder.done();
            message.header().setResponseToMsgId(responseToId);
            return message;
        });
        return cmdObjReceived;
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId) {
        auto cursorResponse = buildCursorResponse(fieldName, start, end, cursorId);
        return scheduleResponse(cursorResponse);
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds) {
        auto cursorResponse = buildMultiCursorResponse(fieldName, start, end, cursorIds);
        return scheduleResponse(cursorResponse);
    }

    void scheduleErrorResponse(Status error) {
        scheduleResponse(error);
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId) {

        auto cursorResponse =
            BSON("cursorsKilled" << BSON_ARRAY((long long)(cursorId)) << "cursorsNotFound"
                                 << BSONArray() << "cursorsAlive" << BSONArray() << "cursorsUnknown"
                                 << BSONArray() << "ok" << 1);
        return scheduleResponse(cursorResponse);
    }

    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        options.pinConnection = true;
        return TaskExecutorCursor(getExecutorPtr(), rcr, std::move(options));
    }

    bool hasReadyRequests() {
        return asBase().hasReadyRequests();
    }

    void blackHoleNextOutgoingRequest() {
        auto pf = makePromiseFuture<void>();
        expectSinkMessage([&](Message m) {
            pf.promise.emplaceValue();
            return Status(ErrorCodes::SocketException, "test");
        });
        pf.future.get();
    }
};

class NoPrefetchTaskExecutorCursorTestFixture : public NonPinningTaskExecutorCursorTestFixture {
public:
    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        options.preFetchNextBatch = false;
        return TaskExecutorCursor(getExecutorPtr(), rcr, std::move(options));
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        // Don't assert that the network has requests like we do in other classes. This is to enable
        // the test in 'NoPrefetchGetMore'.
        auto rcr =
            ing->scheduleSuccessfulResponse(buildCursorResponse(fieldName, start, end, cursorId));
        ing->runReadyNetworkOperations();
        return rcr.cmdObj.getOwned();
    }
};

class NoPrefetchPinnedTaskExecutorCursorTestFixture
    : public PinnedConnTaskExecutorCursorTestFixture {
public:
    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        options.preFetchNextBatch = false;
        options.pinConnection = true;
        return TaskExecutorCursor(getExecutorPtr(), rcr, std::move(options));
    }
};

TEST_F(NonPinningTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

/**
 * Ensure early termination of the cursor calls killCursor (if we know about the cursor id)
 * Only applicable to the unpinned case - if the connection is pinned, and a getMore is
 * in progress and/or fails, the most we can do is kill the connection. We can't re-use
 * the connection to send killCursors.
 */
TEST_F(NonPinningTaskExecutorCursorTestFixture, EarlyReturnKillsCursor) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"),
                             DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             findCmd,
                             opCtx.get());

    {
        TaskExecutorCursor tec = makeTec(rcr);

        scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

        ASSERT(tec.getNext(opCtx.get()));

        // Black hole the pending `getMore` operation scheduled by the `TaskExecutorCursor`.
        blackHoleNextOutgoingRequest();
    }


    ASSERT_BSONOBJ_EQ(BSON("killCursors"
                           << "test"
                           << "cursors" << BSON_ARRAY(1)),
                      scheduleSuccessfulKillCursorResponse(1));
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

/**
 * Ensure the LSID is passed in all stages of querying. Need to test the
 * pinning case separately because of difference around killCursor.
 */
TEST_F(NonPinningTaskExecutorCursorTestFixture, LsidIsPassed) {
    auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);

    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 1);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"),
                             DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             findCmd,
                             opCtx.get());

    boost::optional<TaskExecutorCursor> tec;
    tec.emplace(makeTec(rcr, []() {
        TaskExecutorCursor::Options opts;
        opts.batchSize = 1;
        return opts;
    }()));

    // lsid in the first batch
    ASSERT_BSONOBJ_EQ(BSON("find"
                           << "test"
                           << "batchSize" << 1 << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulCursorResponse("firstBatch", 1, 1, cursorId));

    ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

    // lsid in the getmore
    ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                     << "test"
                                     << "batchSize" << 1 << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulCursorResponse("nextBatch", 2, 2, cursorId));

    tec.reset();

    // lsid in the killcursor
    ASSERT_BSONOBJ_EQ(BSON("killCursors"
                           << "test"
                           << "cursors" << BSON_ARRAY(1) << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulKillCursorResponse(1));

    ASSERT_FALSE(hasReadyRequests());
}

TEST_F(NoPrefetchTaskExecutorCursorTestFixture, NoPrefetchGetMore) {
    NoPrefetchGetMore();
}

TEST_F(NoPrefetchPinnedTaskExecutorCursorTestFixture, NoPrefetchWithPinning) {
    NoPrefetchGetMore();
}

}  // namespace
}  // namespace executor
}  // namespace mongo
