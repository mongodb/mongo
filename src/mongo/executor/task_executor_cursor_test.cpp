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


#include "mongo/executor/task_executor_cursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/executor/task_executor_cursor_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <climits>
#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace executor {
namespace {
/**
 * Fixture for the task executor cursor tests.
 *
 * The fixture is templated so that it can operate with the network-mocking needs of either pinned
 * cursors (via PinnedConnTaskExecutorCursorTestFixture) or unpinned cursors (via
 * NonPinningTaskExecutorCursorTestFixture). The tests defined within this fixture can then be run
 * in either configuration.
 */
template <typename Base>
class TaskExecutorCursorTestFixture : public Base {
public:
    void setUp() override {
        Base::setUp();
        client = serviceCtx->getService()->makeClient("TaskExecutorCursorTest");
        opCtx = client->makeOperationContext();
        Base::postSetUp();
    }

    void tearDown() override {
        opCtx.reset();
        client.reset();

        Base::tearDown();
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId,
                                             bool expectedPrefetch = true) {
        return Base::scheduleSuccessfulCursorResponse(
            fieldName, start, end, cursorId, expectedPrefetch);
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds,
                                                  bool expectedPrefetch = true) {
        return Base::scheduleSuccessfulMultiCursorResponse(
            fieldName, start, end, cursorIds, expectedPrefetch);
    }

    void scheduleErrorResponse(Status error) {
        return Base::scheduleErrorResponse(error);
    }
    void blackHoleNextOutgoingRequest() {
        return Base::blackHoleNextOutgoingRequest();
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId, bool expectedPrefetch = true) {
        return Base::scheduleSuccessfulKillCursorResponse(cursorId, expectedPrefetch);
    }

    std::unique_ptr<TaskExecutorCursor> makeTec(RemoteCommandRequest rcr,
                                                boost::optional<int64_t> batchSize = boost::none,
                                                bool preFetchNextBatch = true) {
        // pinConnection is a required argument to construct the options, but will be overriden in
        // the base class to the correct value.
        TaskExecutorCursorOptions options(/*pinConnection*/ false, batchSize, preFetchNextBatch);
        return Base::makeTec(rcr, std::move(options));
    }

    bool hasReadyRequests() {
        return Base::hasReadyRequests();
    }

    /**
     * Waits for up to 10 seconds for an expected ready request. If we wake up because there is a
     * ready request, return true; otherwise, return false.
     *
     * The non-pinning test fixture schedules and receives requests synchronously, so no wait is
     * necessary in its implementation.
     */
    bool tryWaitUntilReadyRequests() {
        return Base::tryWaitUntilReadyRequests();
    }

    /**
     * Ensure we work for a single simple batch
     */
    void SingleBatchWorksTest() {
        const auto findCmd = BSON("find" << "test"
                                         << "batchSize" << 2);
        const CursorId cursorId = 0;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        auto tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId));

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_FALSE(hasReadyRequests());

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec->getNext(opCtx.get()));
    }

    /**
     * Ensure the firstBatch can be read correctly when multiple cursors are returned.
     */
    void MultipleCursorsSingleBatchSucceedsTest() {
        const auto aggCmd =
            BSON("aggregate" << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 aggCmd,
                                 opCtx.get());

        auto tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(aggCmd,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, {0, 0}));

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec->getNext(opCtx.get()));

        auto cursorVec = tec->releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(secondCursor->getNext(opCtx.get()));
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
        const auto aggCmd =
            BSON("aggregate" << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true)));
        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 aggCmd,
                                 opCtx.get());
        auto tec = makeTec(rcr);
        auto expected =
            BSON("aggregate" << "test"
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
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec->getNext(opCtx.get()));

        auto cursorVec = tec->releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(secondCursor->getNext(opCtx.get()));
    }

    /**
     * Tests that TaskExecutorCursors that share PinnedConnectionTaskExecutors can be destroyed
     * without impacting/canceling work on other TaskExecutorCursors. See SERVER-93583 for details.
     */
    void CancelTECWhileSharedPCTEInUse() {
        const auto aggCmd =
            BSON("aggregate" << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

        std::vector<size_t> cursorIds{1, 2};
        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 aggCmd,
                                 opCtx.get());
        std::vector<std::unique_ptr<TaskExecutorCursor>> cursorVec;
        {
            auto tec = makeTec(rcr);

            ASSERT_BSONOBJ_EQ(aggCmd,
                              scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, cursorIds));
            // Get data from cursor.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

            cursorVec = tec->releaseAdditionalCursors();
            ASSERT_EQUALS(cursorVec.size(), 1);
            // Destroy initial cursor.
        }
        // Schedule EOF on the first cursor to satisfy the prefetch and show that the operation can
        // safely come back error-free.
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 3, 3, 0));

        auto secondCursor = std::move(cursorVec[0]);
        // Fetch first set of pre-fetched data from the second cursor.
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 4);

        // Next, respond to the outstanding getMore requests on the secondCursor. This would be
        // impossible if the underlying executor was cancelled.
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 2LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 6, 8, cursorIds[1]));

        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 6);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 7);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 8);

        // Next, a killCursor command is scheduled by the destructor of the first TEC (AFTER
        // successful completion of its outstanding operations) to ensure we don't leak that cursor.
        ASSERT_BSONOBJ_EQ(BSON("killCursors" << "test"
                                             << "cursors" << BSON_ARRAY((int)cursorIds[0])),
                          scheduleSuccessfulKillCursorResponse(cursorIds[0]));

        // Finally, schedule EOF on the second cursor.
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 2LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 12, 12, 0));
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 12);

        // There are no outstanding requests and the second cursor is closed.
        ASSERT_FALSE(hasReadyRequests());
        ASSERT_FALSE(secondCursor->getNext(opCtx.get()));
    }

    void MultipleCursorsGetMoreWorksTest() {
        const auto aggCmd =
            BSON("aggregate" << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

        std::vector<size_t> cursorIds{1, 2};
        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 aggCmd,
                                 opCtx.get());

        auto tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(aggCmd,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, cursorIds));

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

        auto cursorVec = tec->releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);

        // If we try to getNext() at this point, we are interruptible and can timeout
        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { tec->getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        // We can pick up after that interruption though
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 3, 5, cursorIds[0]));

        // Repeat for second cursor.
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 4);

        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { secondCursor->getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        ASSERT_BSONOBJ_EQ(BSON("getMore" << 2LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 6, 8, cursorIds[1]));
        // Read second batch, then schedule EOF on both cursors.
        // Then read final document for each.
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 3);
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 5);
        scheduleSuccessfulCursorResponse("nextBatch", 6, 6, 0);
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 6);

        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 6);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 7);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 8);
        scheduleSuccessfulCursorResponse("nextBatch", 12, 12, 0);
        ASSERT_EQUALS(secondCursor->getNext(opCtx.get()).value()["x"].Int(), 12);

        // Shouldn't have any more requests, both cursors are closed.
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(tec->getNext(opCtx.get()));
        ASSERT_FALSE(secondCursor->getNext(opCtx.get()));
    }

    /**
     * Ensure we work if find fails (and that we receive the error code it failed with)
     */
    void FailureInFindTest() {
        const auto findCmd = BSON("find" << "test"
                                         << "batchSize" << 2);

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        auto tec = makeTec(rcr);

        scheduleErrorResponse(Status(ErrorCodes::BadValue, "an error"));

        ASSERT_THROWS_CODE(tec->getNext(opCtx.get()), DBException, ErrorCodes::BadValue);
    }


    /**
     * Ensure multiple batches works correctly
     */
    void MultipleBatchesWorksTest() {
        const auto findCmd = BSON("find" << "test"
                                         << "batchSize" << 2);
        CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        auto tec = makeTec(rcr, /*batchSize*/ 3);

        scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

        // ASSERT(hasReadyRequests());

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

        // If we try to getNext() at this point, we are interruptible and can timeout
        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { tec->getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        // We can pick up after that interruption though
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                         << "test"
                                         << "batchSize" << 3),
                          scheduleSuccessfulCursorResponse("nextBatch", 3, 5, cursorId));

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 3);
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 5);

        cursorId = 0;
        scheduleSuccessfulCursorResponse("nextBatch", 6, 6, cursorId);

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 6);

        // We don't issue extra getmores after returning a 0 cursor id
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(tec->getNext(opCtx.get()));
    }

    /**
     * Ensure we allow empty firstBatch.
     */
    void EmptyFirstBatchTest() {
        const auto findCmd = BSON("find" << "test"
                                         << "batchSize" << 2);
        const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                               << "test"
                                               << "batchSize" << 3);
        const CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        auto tec = makeTec(rcr, /*batchSize*/ 3);

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
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

        th.join();
    }

    /**
     * Ensure we allow any empty non-initial batch.
     */
    void EmptyNonInitialBatchTest() {
        const auto findCmd = BSON("find" << "test"
                                         << "batchSize" << 2);
        const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                               << "test"
                                               << "batchSize" << 3);
        const CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 findCmd,
                                 opCtx.get());

        auto tec = makeTec(rcr, /*batchSize*/ 3);

        // Schedule a cursor response with a non-empty "firstBatch".
        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 1, cursorId));

        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

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
        ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

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
                                     BSON("search" << "foo"),
                                     opCtx.get());

            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeTec(rcr, /*batchSize*/ 2, /*preFetchNextBatch*/ false);

            // Mock the response for the first batch.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 2, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

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
                auto recievedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 3, 4, 0, /*expectedPrefetch*/ false);

                // Assert that the command processed for the above response matches with the
                // lambda to augment the getMore command used during construction of the TEC
                // above.
                const auto expectedGetMoreCmd = BSON("getMore" << 1LL << "collection"
                                                               << "test"
                                                               << "batchSize" << 2);
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, recievedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 3);
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 4);

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

using NonPinningDefaultTaskExecutorCursorTestFixture =
    TaskExecutorCursorTestFixture<NonPinningTaskExecutorCursorTestFixture>;
using PinnedConnDefaultTaskExecutorCursorTestFixture =
    TaskExecutorCursorTestFixture<PinnedConnTaskExecutorCursorTestFixture>;

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

/**
 * Ensure early termination of the cursor calls killCursor (if we know about the cursor id)
 * Only applicable to the unpinned case - if the connection is pinned, and a getMore is
 * in progress and/or fails, the most we can do is kill the connection. We can't re-use
 * the connection to send killCursors.
 */
TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, EarlyReturnKillsCursor) {
    const auto findCmd = BSON("find" << "test"
                                     << "batchSize" << 2);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"),
                             DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             findCmd,
                             opCtx.get());

    {
        auto tec = makeTec(rcr);

        scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

        ASSERT(tec->getNext(opCtx.get()));

        // Black hole the pending `getMore` operation scheduled by the `TaskExecutorCursor`.
        blackHoleNextOutgoingRequest();
    }


    ASSERT_BSONOBJ_EQ(BSON("killCursors" << "test"
                                         << "cursors" << BSON_ARRAY(1)),
                      scheduleSuccessfulKillCursorResponse(1));
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

/**
 * Ensure the LSID is passed in all stages of querying. Need to test the
 * pinning case separately because of difference around killCursor.
 */
TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, LsidIsPassed) {
    auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);

    const auto findCmd = BSON("find" << "test"
                                     << "batchSize" << 1);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"),
                             DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             findCmd,
                             opCtx.get());

    std::unique_ptr<TaskExecutorCursor> tec;
    tec = makeTec(rcr, /*batchSize*/ 1);

    // lsid in the first batch
    ASSERT_BSONOBJ_EQ(BSON("find" << "test"
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
    ASSERT_BSONOBJ_EQ(BSON("killCursors" << "test"
                                         << "cursors" << BSON_ARRAY(1) << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulKillCursorResponse(1));

    ASSERT_FALSE(hasReadyRequests());
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, NoPrefetchGetMore) {
    NoPrefetchGetMore();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, NoPrefetchWithPinning) {
    NoPrefetchGetMore();
}

TEST_F(PinnedConnDefaultTaskExecutorCursorTestFixture, MultipleCursorsCancellation) {
    CancelTECWhileSharedPCTEInUse();
}

TEST_F(NonPinningDefaultTaskExecutorCursorTestFixture, MultipleCursorsCancellation) {
    // For good measure, run this test in non-pinned mode as well. This test was motivated by
    // SERVER-93583, which exposed a bug in pinning mode, but should pass in both modes.
    CancelTECWhileSharedPCTEInUse();
}

}  // namespace
}  // namespace executor
}  // namespace mongo
