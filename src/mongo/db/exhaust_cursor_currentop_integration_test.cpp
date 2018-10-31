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

#include "mongo/db/query/cursor_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {
namespace {
// Obtain a pointer to the global system clock. Used to enforce timeouts in the parallel thread.
auto* const clock = SystemClockSource::get();

std::unique_ptr<DBClientBase> connect(StringData appName) {
    std::string errMsg;
    auto conn = unittest::getFixtureConnectionString().connect(appName.toString(), errMsg);
    uassert(ErrorCodes::SocketException, errMsg, conn);
    return conn;
}

void setFailpoint(DBClientBase* conn, StringData failPoint, bool enable) {
    auto cmdObj = BSON("configureFailPoint" << failPoint.toString() << "mode"
                                            << (enable ? "alwaysOn" : "off"));
    auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", cmdObj));
    ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
}

bool confirmCurrentOpContents(DBClientBase* conn,
                              BSONObj curOpMatch,
                              Milliseconds timeoutMS,
                              Milliseconds intervalMS = Milliseconds(200)) {
    auto curOpCmd = BSON(
        "aggregate" << 1 << "cursor" << BSONObj() << "pipeline"
                    << BSON_ARRAY(BSON("$currentOp" << BSONObj()) << BSON("$match" << curOpMatch)));
    const auto startTime = clock->now();
    while (clock->now() - startTime < timeoutMS) {
        auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", curOpCmd));
        auto swCursorRes = CursorResponse::parseFromBSON(reply->getCommandReply());
        ASSERT_OK(swCursorRes.getStatus());
        if (!swCursorRes.getValue().getBatch().empty()) {
            return true;
        }
        sleepFor(intervalMS);
    }
    return false;
}
}  // namespace

TEST(CurrentOpExhaustCursorTest, CanSeeEachExhaustCursorPseudoGetMoreInCurrentOpOutput) {
    const NamespaceString testNSS{"exhaust_cursor_currentop.exhaust_cursor_currentop"};
    auto conn = connect("curop_exhaust_cursor_test");

    // Specifies the amount of time we are willing to wait for a parallel operation to complete.
    const auto parallelWaitTimeoutMS = Milliseconds(5 * 60 * 1000);

    // We need to set failpoints around getMore which cause it to hang, so only test against a
    // single server rather than a replica set or mongoS.
    if (conn->isReplicaSetMember() || conn->isMongos()) {
        return;
    }

    // Drop and recreate the test namespace.
    conn->dropCollection(testNSS.ns());
    for (int i = 0; i < 10; i++) {
        auto insertCmd =
            BSON("insert" << testNSS.coll() << "documents" << BSON_ARRAY(BSON("a" << i)));
        auto reply = conn->runCommand(OpMsgRequest::fromDBAndBody(testNSS.db(), insertCmd));
        ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
    }

    // Enable a failpoint to block getMore during execution.
    setFailpoint(conn.get(), "waitWithPinnedCursorDuringGetMoreBatch", true);

    // Execute a query on a separate thread, with the 'exhaust' flag set.
    std::unique_ptr<DBClientBase> queryConnection;
    std::unique_ptr<DBClientCursor> queryCursor;
    auto queryThread = stdx::async(stdx::launch::async, [&testNSS, &queryConnection, &queryCursor] {
        queryConnection = connect("curop_exhaust_cursor_test_bg");
        auto projSpec = BSON("_id" << 0 << "a" << 1);
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

    // Ensure that, regardless of whether the test completes or fails, we release all failpoints.
    ON_BLOCK_EXIT([&conn, &queryThread] {
        setFailpoint(conn.get(), "waitWithPinnedCursorDuringGetMoreBatch", false);
        setFailpoint(conn.get(), "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch", false);
        queryThread.wait();
    });

    // Wait until the parallel operation initializes its cursor.
    const auto startTime = clock->now();
    while (!queryCursor && (clock->now() - startTime < parallelWaitTimeoutMS)) {
        sleepFor(Milliseconds(10));
    }
    ASSERT(queryCursor);

    // We expect that the server, having received the first {batchSize: 1} getMore for the parallel
    // thread's exhaust cursor, will produce a series of pseudo-getMores internally and stream the
    // results back to the client until the cursor is exhausted. Here, we verify that each of these
    // pseudo-getMores appear in the $currentOp output.
    for (int i = 2; i < 10; ++i) {
        // Generate a currentOp filter based on the cursorId and the cumulative nDocsReturned.
        const auto curOpMatch = BSON("command.collection"
                                     << "exhaust_cursor_currentop"
                                     << "command.getMore"
                                     << queryCursor->getCursorId()
                                     << "msg"
                                     << "waitWithPinnedCursorDuringGetMoreBatch"
                                     << "cursor.nDocsReturned"
                                     << i);

        // Confirm that the exhaust getMore appears in the $currentOp output.
        ASSERT(confirmCurrentOpContents(conn.get(), curOpMatch, parallelWaitTimeoutMS));

        // Airlock the failpoint by releasing it only after we enable a post-getMore failpoint. This
        // ensures that no subsequent getMores can run before we re-enable the original failpoint.
        setFailpoint(conn.get(), "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch", true);
        setFailpoint(conn.get(), "waitWithPinnedCursorDuringGetMoreBatch", false);
        // Confirm that the getMore completed its batch and hit the post-getMore failpoint.
        ASSERT(confirmCurrentOpContents(
            conn.get(),
            BSON("command.getMore" << queryCursor->getCursorId() << "msg"
                                   << "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch"),
            parallelWaitTimeoutMS));
        // Re-enable the original failpoint to catch the next getMore, and release the current one.
        setFailpoint(conn.get(), "waitWithPinnedCursorDuringGetMoreBatch", true);
        setFailpoint(conn.get(), "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch", false);
    }
}
}  // namespace mongo
