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


#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {
namespace {

/**
 * Fixture for the task executor cursor tests which offers some convenience methods to help with
 * scheduling responses
 */
class TaskExecutorCursorFixture : public ThreadPoolExecutorTest {
public:
    TaskExecutorCursorFixture() {
        serviceCtx->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
    }

    void setUp() override {
        ThreadPoolExecutorTest::setUp();

        client = serviceCtx->makeClient("TaskExecutorCursorTest");
        opCtx = client->makeOperationContext();

        launchExecutorThread();
    }

    void tearDown() override {
        opCtx.reset();
        client.reset();

        ThreadPoolExecutorTest::tearDown();
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

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

        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(bob.obj());
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

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

        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(bob.obj());
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

    bool hasReadyRequests() {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        return getNet()->hasReadyRequests();
    }

    ServiceContext::UniqueServiceContext serviceCtx = ServiceContext::make();
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

/**
 * Ensure we work for a single simple batch
 */
TEST_F(TaskExecutorCursorFixture, SingleBatchWorks) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);
    const CursorId cursorId = 0;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    TaskExecutorCursor tec(&getExecutor(), rcr);

    ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId));

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 1);

    ASSERT_FALSE(hasReadyRequests());

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 2);

    ASSERT_FALSE(tec.getNext(opCtx.get()));
}

/**
 * Ensure the firstBatch can be read correctly when multiple cursors are returned.
 */
TEST_F(TaskExecutorCursorFixture, MultipleCursorsSingleBatchSucceeds) {
    const auto aggCmd = BSON("aggregate"
                             << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", aggCmd, opCtx.get());

    TaskExecutorCursor tec(&getExecutor(), rcr);

    ASSERT_BSONOBJ_EQ(aggCmd, scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, {0, 0}));

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 1);

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 2);

    ASSERT_FALSE(tec.getNext(opCtx.get()));

    auto cursorVec = tec.releaseAdditionalCursors();
    ASSERT_EQUALS(cursorVec.size(), 1);
    auto secondCursor = std::move(cursorVec[0]);

    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 2);
    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 4);
    ASSERT_FALSE(hasReadyRequests());

    ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
}

TEST_F(TaskExecutorCursorFixture, MultipleCursorsGetMoreWorks) {
    const auto aggCmd = BSON("aggregate"
                             << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

    std::vector<size_t> cursorIds{1, 2};
    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", aggCmd, opCtx.get());

    TaskExecutorCursor tec(&getExecutor(), rcr);

    ASSERT_BSONOBJ_EQ(aggCmd, scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, cursorIds));

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 1);

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 2);

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

    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 2);
    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 4);

    ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                              ErrorCodes::ExceededTimeLimit,
                                              [&] { secondCursor.getNext(opCtx.get()); }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);

    ASSERT_BSONOBJ_EQ(BSON("getMore" << 2LL << "collection"
                                     << "test"),
                      scheduleSuccessfulCursorResponse("nextBatch", 6, 8, cursorIds[1]));
    // Read second batch on both cursors.
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 3);
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 4);
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 5);
    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 6);
    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 7);
    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 8);

    // Schedule EOF on both cursors.
    scheduleSuccessfulCursorResponse("nextBatch", 6, 6, 0);
    scheduleSuccessfulCursorResponse("nextBatch", 12, 12, 0);

    // Read final document.
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 6);
    ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).get()["x"].Int(), 12);

    // Shouldn't have any more requests, both cursors are closed.
    ASSERT_FALSE(hasReadyRequests());

    ASSERT_FALSE(tec.getNext(opCtx.get()));
    ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
}

/**
 * Ensure we work if find fails (and that we receive the error code it failed with)
 */
TEST_F(TaskExecutorCursorFixture, FailureInFind) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    TaskExecutorCursor tec(&getExecutor(), rcr);

    {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        ASSERT(getNet()->hasReadyRequests());
        getNet()->scheduleErrorResponse(Status(ErrorCodes::BadValue, "an error"));
        getNet()->runReadyNetworkOperations();
    }

    ASSERT_THROWS_CODE(tec.getNext(opCtx.get()), DBException, ErrorCodes::BadValue);
}

/**
 * Ensure early termination of the cursor calls killCursor (if we know about the cursor id)
 */
TEST_F(TaskExecutorCursorFixture, EarlyReturnKillsCursor) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    {
        TaskExecutorCursor tec(&getExecutor(), rcr);

        scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

        ASSERT(tec.getNext(opCtx.get()));
    }

    // Black hole the pending `getMore` operation scheduled by the `TaskExecutorCursor`.
    {
        NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->blackHole(getNet()->getFrontOfUnscheduledQueue());
    }

    ASSERT_BSONOBJ_EQ(BSON("killCursors"
                           << "test"
                           << "cursors" << BSON_ARRAY(1)),
                      scheduleSuccessfulKillCursorResponse(1));
}

/**
 * Ensure multiple batches works correctly
 */
TEST_F(TaskExecutorCursorFixture, MultipleBatchesWorks) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);
    CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    TaskExecutorCursor tec(&getExecutor(), rcr, [] {
        TaskExecutorCursor::Options opts;
        opts.batchSize = 3;
        return opts;
    }());

    scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 1);

    ASSERT(hasReadyRequests());

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 2);

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

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 3);
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 4);
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 5);

    cursorId = 0;
    scheduleSuccessfulCursorResponse("nextBatch", 6, 6, cursorId);

    // We don't issue extra getmores after returning a 0 cursor id
    ASSERT_FALSE(hasReadyRequests());

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 6);

    ASSERT_FALSE(tec.getNext(opCtx.get()));
}

/**
 * Ensure we allow empty firstBatch.
 */
TEST_F(TaskExecutorCursorFixture, EmptyFirstBatch) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);
    const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                           << "test"
                                           << "batchSize" << 3);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    TaskExecutorCursor tec(&getExecutor(), rcr, [] {
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

        ASSERT_BSONOBJ_EQ(getMoreCmd,
                          scheduleSuccessfulCursorResponse("nextBatch", 1, 1, cursorId));
    });

    // Verify that the first doc is the doc from the second batch.
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 1);

    th.join();
}

/**
 * Ensure we allow any empty non-initial batch.
 */
TEST_F(TaskExecutorCursorFixture, EmptyNonInitialBatch) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);
    const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                           << "test"
                                           << "batchSize" << 3);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    TaskExecutorCursor tec(&getExecutor(), rcr, [] {
        TaskExecutorCursor::Options opts;
        opts.batchSize = 3;
        return opts;
    }());

    // Schedule a cursor response with a non-empty "firstBatch".
    ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 1, cursorId));

    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 1);

    // Schedule two consecutive cursor responses with empty "nextBatch". Use end < start so
    // we don't append any doc to "nextBatch".
    ASSERT_BSONOBJ_EQ(getMoreCmd, scheduleSuccessfulCursorResponse("nextBatch", 1, 0, cursorId));

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

        ASSERT_BSONOBJ_EQ(getMoreCmd,
                          scheduleSuccessfulCursorResponse("nextBatch", 2, 2, cursorId));
    });

    // Verify that the next doc is the doc from the fourth batch.
    ASSERT_EQUALS(tec.getNext(opCtx.get()).get()["x"].Int(), 2);

    th.join();
}

/**
 * Ensure lsid is passed in all stages of querying
 */
TEST_F(TaskExecutorCursorFixture, LsidIsPassed) {
    auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);

    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 1);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    boost::optional<TaskExecutorCursor> tec;
    tec.emplace(&getExecutor(), rcr, []() {
        TaskExecutorCursor::Options opts;
        opts.batchSize = 1;
        return opts;
    }());

    // lsid in the first batch
    ASSERT_BSONOBJ_EQ(BSON("find"
                           << "test"
                           << "batchSize" << 1 << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulCursorResponse("firstBatch", 1, 1, cursorId));

    ASSERT_EQUALS(tec->getNext(opCtx.get()).get()["x"].Int(), 1);

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

}  // namespace
}  // namespace executor
}  // namespace mongo
