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

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {
namespace {
// Specifies the amount of time we are willing to wait for a parallel operation to complete.
const auto parallelWaitTimeoutMS = Milliseconds(5 * 60 * 1000);

// Obtain a pointer to the global system clock. Used to enforce timeouts in the parallel thread.
auto* const clock = SystemClockSource::get();

const NamespaceString testNSS{"exhaust_cursor_currentop.testColl"};

const StringData testAppName = "curop_exhaust_cursor_test";
std::unique_ptr<DBClientBase> connect(StringData appName = testAppName) {
    std::string errMsg;
    auto conn = unittest::getFixtureConnectionString().connect(appName.toString(), errMsg);
    uassert(ErrorCodes::SocketException, errMsg, conn);
    return conn;
}
const StringData testBackgroundAppName = "curop_exhaust_cursor_test_bg";

void initTestCollection(DBClientBase* conn) {
    // Drop and recreate the test namespace.
    conn->dropCollection(testNSS.ns());
    for (int i = 0; i < 10; i++) {
        auto insertCmd =
            BSON("insert" << testNSS.coll() << "documents" << BSON_ARRAY(BSON("a" << i)));
        auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody(testNSS.db(), insertCmd));
        ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
    }
}

void setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(DBClientBase* conn, bool enable) {
    auto cmdObj = BSON("configureFailPoint"
                       << "waitWithPinnedCursorDuringGetMoreBatch"
                       << "mode" << (enable ? "alwaysOn" : "off") << "data"
                       << BSON("shouldNotdropLock" << true << "shouldContinueOnInterrupt" << true));
    auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", cmdObj));
    ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
}

void setWaitAfterCommandFinishesExecutionFailpoint(DBClientBase* conn, bool enable) {
    auto cmdObj = BSON("configureFailPoint"
                       << "waitAfterCommandFinishesExecution"
                       << "mode" << (enable ? "alwaysOn" : "off") << "data"
                       << BSON("ns" << testNSS.toString()));
    auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", cmdObj));
    ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
}

void setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(DBClientBase* conn,
                                                                      bool enable) {
    auto cmdObj = BSON("configureFailPoint"
                       << "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch"
                       << "mode" << (enable ? "alwaysOn" : "off"));
    auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", cmdObj));
    ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
}

bool confirmCurrentOpContents(DBClientBase* conn,
                              BSONObj curOpMatch,
                              bool expectEmptyResult = false,
                              Milliseconds timeoutMS = Milliseconds(5 * 60 * 1000),
                              Milliseconds intervalMS = Milliseconds(200)) {
    auto curOpCmd = BSON("aggregate" << 1 << "cursor" << BSONObj() << "pipeline"
                                     << BSON_ARRAY(BSON("$currentOp" << BSON("idleCursors" << true))
                                                   << BSON("$match" << curOpMatch)));
    const auto startTime = clock->now();
    while (clock->now() - startTime < timeoutMS) {
        auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", curOpCmd));
        auto swCursorRes = CursorResponse::parseFromBSON(reply->getCommandReply());
        ASSERT_OK(swCursorRes.getStatus());
        if (swCursorRes.getValue().getBatch().empty() == expectEmptyResult) {
            return true;
        }
        sleepFor(intervalMS);
    }
    return false;
}

// Start an exhaust request with a batchSize of 2 in the initial 'find' and a batchSize of 1 in
// subsequent 'getMore's.
auto startExhaustQuery(DBClientBase* queryConnection,
                       std::unique_ptr<DBClientCursor>& queryCursor) {
    auto queryThread = stdx::async(stdx::launch::async, [&] {
        const auto projSpec = BSON("_id" << 0 << "a" << 1);
        // Issue the initial 'find' with a batchSize of 2 and the exhaust flag set. We then iterate
        // through the first batch and confirm that the results are as expected.
        queryCursor = queryConnection->query(testNSS, {}, 0, 0, &projSpec, QueryOption_Exhaust, 2);
        for (int i = 0; i < 2; ++i) {
            ASSERT_BSONOBJ_EQ(queryCursor->nextSafe(), BSON("a" << i));
        }
        // Having exhausted the two results returned by the initial find, we set the batchSize to 1
        // and issue a single getMore via DBClientCursor::more(). Because the 'exhaust' flag is set,
        // the server will generate a series of internal getMores and stream them back to the client
        // until the cursor is exhausted, without the client sending any further getMore requests.
        // We expect this request to hang at the 'waitWithPinnedCursorDuringGetMoreBatch' failpoint.
        queryCursor->setBatchSize(1);
        ASSERT(queryCursor->more());
    });

    // Wait until the parallel operation initializes its cursor.
    const auto startTime = clock->now();
    while (!queryCursor && (clock->now() - startTime < parallelWaitTimeoutMS)) {
        sleepFor(Milliseconds(10));
    }
    ASSERT(queryCursor);
    unittest::log() << "Started exhaust query with cursorId: " << queryCursor->getCursorId();
    return queryThread;
}

void runOneGetMore(DBClientBase* conn,
                   const std::unique_ptr<DBClientCursor>& queryCursor,
                   int nDocsReturned) {
    const auto curOpMatch = BSON("command.collection" << testNSS.coll() << "command.getMore"
                                                      << queryCursor->getCursorId() << "msg"
                                                      << "waitWithPinnedCursorDuringGetMoreBatch"
                                                      << "cursor.nDocsReturned" << nDocsReturned);
    // Confirm that the initial getMore appears in the $currentOp output.
    ASSERT(confirmCurrentOpContents(conn, curOpMatch));

    // Airlock the failpoint by releasing it only after we enable a post-getMore failpoint. This
    // ensures that no subsequent getMores can run before we re-enable the original failpoint.
    setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn, true);
    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn, false);
    // Confirm that the getMore completed its batch and hit the post-getMore failpoint.
    ASSERT(confirmCurrentOpContents(
        conn,
        BSON("command.getMore" << queryCursor->getCursorId() << "msg"
                               << "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch")));

    // Re-enable the original failpoint to catch the next getMore, and release the current one.
    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn, true);
    setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn, false);

    ASSERT(queryCursor->more());
    // Assuming documents start with {a: 0}, the (nDocsReturned+1)-th document should have {a:
    // nDocsReturned}. See initTestCollection().
    ASSERT_BSONOBJ_EQ(queryCursor->nextSafe(), BSON("a" << nDocsReturned));
}
}  // namespace

TEST(CurrentOpExhaustCursorTest, CanSeeEachExhaustCursorPseudoGetMoreInCurrentOpOutput) {
    auto conn = connect();

    // We need to set failpoints around getMore which cause it to hang, so only test against a
    // single server rather than a replica set or mongoS.
    if (conn->isReplicaSetMember() || conn->isMongos()) {
        return;
    }

    initTestCollection(conn.get());

    // Enable a failpoint to block getMore during execution.
    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), true);

    const auto queryConnection = connect(testBackgroundAppName);
    std::unique_ptr<DBClientCursor> queryCursor;

    // Execute a query on a separate thread, with the 'exhaust' flag set.
    auto queryThread = startExhaustQuery(queryConnection.get(), queryCursor);
    // Ensure that, regardless of whether the test completes or fails, we release all failpoints.
    ON_BLOCK_EXIT([&conn, &queryThread] {
        setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), false);
        setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn.get(), false);
        queryThread.wait();
    });

    // We expect that the server, having received the first {batchSize: 1} getMore for the parallel
    // thread's exhaust cursor, will produce a series of pseudo-getMores internally and stream the
    // results back to the client until the cursor is exhausted. Here, we verify that each of these
    // pseudo-getMores appear in the $currentOp output.
    for (int i = 2; i < 10; ++i) {
        runOneGetMore(conn.get(), queryCursor, i);
    }
}

// Test exhaust cursor is cleaned up on client disconnect. By default, the test client disconnects
// while the exhaust getMore is running. If disconnectAfterGetMoreBatch is set to true, the test
// client disconnects after the exhaust getMore is run but before the server sends out the response.
void testClientDisconnect(bool disconnectAfterGetMoreBatch) {
    auto conn = connect();

    // We need to set failpoints around getMore which cause it to hang, so only test against a
    // single server rather than a replica set or mongoS.
    if (conn->isReplicaSetMember() || conn->isMongos()) {
        return;
    }

    initTestCollection(conn.get());

    // Enable a failpoint to block getMore during execution.
    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), true);

    const auto connStr = unittest::getFixtureConnectionString();
    const auto queryConnection = std::make_unique<DBClientConnection>();
    uassertStatusOK(queryConnection->connect(connStr.getServers()[0], testBackgroundAppName));
    std::unique_ptr<DBClientCursor> queryCursor;

    // Execute a query on a separate thread, with the 'exhaust' flag set.
    auto queryThread = startExhaustQuery(queryConnection.get(), queryCursor);
    // Ensure that, regardless of whether the test completes or fails, we release all failpoints.
    ON_BLOCK_EXIT([&conn, &queryThread] {
        setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), false);
        setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn.get(), false);
        queryThread.wait();
    });

    // This will allow the initial getMore to run.
    runOneGetMore(conn.get(), queryCursor, 2);

    // The next getMore will be an exhaust getMore. Confirm that the exhaust getMore appears in the
    // $currentOp output.
    auto curOpMatch = BSON("command.collection" << testNSS.coll() << "command.getMore"
                                                << queryCursor->getCursorId() << "msg"
                                                << "waitWithPinnedCursorDuringGetMoreBatch"
                                                << "cursor.nDocsReturned" << 3);
    ASSERT(confirmCurrentOpContents(conn.get(), curOpMatch));

    if (disconnectAfterGetMoreBatch) {
        // Allow the exhaust getMore to run but block it before sending out the response.
        setWaitAfterCommandFinishesExecutionFailpoint(conn.get(), true);
        setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), false);
        ASSERT(confirmCurrentOpContents(conn.get(),
                                        BSON("command.getMore"
                                             << queryCursor->getCursorId() << "msg"
                                             << "waitAfterCommandFinishesExecution")));
    }

    // Kill the client connection while the exhaust getMore is blocked on the failpoint.
    queryConnection->shutdownAndDisallowReconnect();
    unittest::log() << "Killed exhaust connection.";

    if (disconnectAfterGetMoreBatch) {
        // Disable the failpoint to allow the exhaust getMore to continue sending out the response
        // after the client disconnects. This will result in a broken pipe error.
        setWaitAfterCommandFinishesExecutionFailpoint(conn.get(), false);
    }

    curOpMatch = BSON("command.collection" << testNSS.coll() << "command.getMore"
                                           << queryCursor->getCursorId());
    // Confirm that the exhaust getMore was interrupted and does not appear in the $currentOp
    // output.
    const bool expectEmptyResult = true;
    ASSERT(confirmCurrentOpContents(conn.get(), curOpMatch, expectEmptyResult));

    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), false);
    setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn.get(), false);

    curOpMatch = BSON("type"
                      << "idleCursor"
                      << "cursor.cursorId" << queryCursor->getCursorId());
    // Confirm that the cursor was cleaned up and does not appear in the $currentOp idleCursor
    // output.
    ASSERT(confirmCurrentOpContents(conn.get(), curOpMatch, expectEmptyResult));
}

TEST(CurrentOpExhaustCursorTest, InterruptExhaustCursorPseudoGetMoreOnClientDisconnect) {
    // Test that an exhaust getMore is interrupted on client disconnect.
    testClientDisconnect(false /* disconnectAfterGetMoreBatch */);
}

TEST(CurrentOpExhaustCursorTest, CleanupExhaustCursorOnBrokenPipe) {
    // Test that exhaust cursor is cleaned up on broken pipe even if the exhaust getMore succeeded.
    testClientDisconnect(true /* disconnectAfterGetMoreBatch */);
}
}  // namespace mongo
