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


#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/optime.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
// Specifies the amount of time we are willing to wait for a parallel operation to complete.
const auto parallelWaitTimeoutMS = stdx::chrono::milliseconds(5 * 60 * 1000);

// Obtain a pointer to the global system clock. Used to enforce timeouts in the parallel thread.
auto* const clock = SystemClockSource::get();

const NamespaceString testNSS =
    NamespaceString::createNamespaceString_forTest("exhaust_cursor_currentop.testColl");

const StringData testAppName = "curop_exhaust_cursor_test";
std::unique_ptr<DBClientBase> connect(StringData appName = testAppName) {
    auto swConn = unittest::getFixtureConnectionString().connect(appName.toString());
    uassertStatusOK(swConn.getStatus());
    return std::move(swConn.getValue());
}
const StringData testBackgroundAppName = "curop_exhaust_cursor_test_bg";

void initTestCollection(DBClientBase* conn) {
    // Drop and recreate the test namespace.
    conn->dropCollection(testNSS);
    for (int i = 0; i < 10; i++) {
        auto insertCmd =
            BSON("insert" << testNSS.coll() << "documents" << BSON_ARRAY(BSON("a" << i)));
        auto reply = conn->runCommand(OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired, testNSS.dbName(), insertCmd));
        ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
    }
}

void setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(DBClientBase* conn, bool enable) {
    auto cmdObj = BSON("configureFailPoint"
                       << "waitWithPinnedCursorDuringGetMoreBatch"
                       << "mode" << (enable ? "alwaysOn" : "off") << "data"
                       << BSON("shouldContinueOnInterrupt" << true));
    auto reply = conn->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmdObj));
    ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
}

void setWaitAfterCommandFinishesExecutionFailpoint(DBClientBase* conn, bool enable) {
    auto cmdObj = BSON("configureFailPoint"
                       << "waitAfterCommandFinishesExecution"
                       << "mode" << (enable ? "alwaysOn" : "off") << "data"
                       << BSON("ns" << testNSS.toString_forTest()));
    auto reply = conn->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmdObj));
    ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
}

void setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(DBClientBase* conn,
                                                                      bool enable) {
    auto cmdObj = BSON("configureFailPoint"
                       << "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch"
                       << "mode" << (enable ? "alwaysOn" : "off"));
    auto reply = conn->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmdObj));
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
        auto reply = conn->runCommand(OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, curOpCmd));
        auto swCursorRes = CursorResponse::parseFromBSON(reply->getCommandReply());
        ASSERT_OK(swCursorRes.getStatus());
        if (swCursorRes.getValue().getBatch().empty() == expectEmptyResult) {
            return true;
        }
        sleepFor(intervalMS);
    }
    auto currentOp = BSON("currentOp" << BSON("idleCursors" << true));
    LOGV2(
        20606,
        "confirmCurrentOpContents fails with curOpMatch: {curOpMatch} currentOp: "
        "{conn_runCommand_OpMsgRequestBuilder_createWithValidatedTenancyScope_admin_currentOp_"
        "getCommandReply}",
        "curOpMatch"_attr = curOpMatch,
        "conn_runCommand_OpMsgRequestBuilder_createWithValidatedTenancyScope_admin_currentOp_getCommandReply"_attr =
            conn->runCommand(OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                                         DatabaseName::kAdmin,
                                                         currentOp))
                ->getCommandReply());
    return false;
}

repl::OpTime getLastAppliedOpTime(DBClientBase* conn) {
    auto reply =
        conn->runCommand(OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                                     DatabaseName::kAdmin,
                                                     BSON("replSetGetStatus" << 1)));
    ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
    auto lastAppliedOpTime = reply->getCommandReply()["optimes"]["appliedOpTime"];
    return repl::OpTime(lastAppliedOpTime["ts"].timestamp(), lastAppliedOpTime["t"].numberLong());
}

// Start an exhaust request with a batchSize of 2 in the initial 'find' and a batchSize of 1 in
// subsequent 'getMore's.
auto startExhaustQuery(
    DBClientBase* queryConnection,
    CursorId& queryCursorId,
    int queryOptions = 0,
    Milliseconds awaitDataTimeoutMS = Milliseconds(5000),
    const boost::optional<repl::OpTime>& lastKnownCommittedOpTime = boost::none) {
    boost::optional<CursorId> cursorId;
    auto cursorIdMutex = MONGO_MAKE_LATCH();  // Protects the 'cursorId' variable.
    stdx::condition_variable cursorIdCV;  // Synchronizes the threads on 'cursorId' initialization.

    auto queryThread = stdx::async(
        stdx::launch::async,
        [&cursorId,
         &cursorIdMutex,
         &cursorIdCV,
         queryConnection,
         queryOptions,
         awaitDataTimeoutMS,
         lastKnownCommittedOpTime] {
            const auto projSpec = BSON("_id" << 0 << "a" << 1);
            // Issue the initial 'find' with a batchSize of 2 and the exhaust flag set.
            // We then iterate through the first batch and confirm that the results are
            // as expected.
            FindCommandRequest findCmd{testNSS};
            findCmd.setProjection(projSpec);
            findCmd.setBatchSize(2);
            if (queryOptions & QueryOption_CursorTailable) {
                findCmd.setTailable(true);
            }
            if (queryOptions & QueryOption_AwaitData) {
                findCmd.setAwaitData(true);
            }

            auto queryCursor =
                queryConnection->find(findCmd, ReadPreferenceSetting{}, ExhaustMode::kOn);
            {
                stdx::lock_guard writeLock(cursorIdMutex);
                cursorId = queryCursor->getCursorId();
            }
            cursorIdCV.notify_one();
            for (int i = 0; i < 2; ++i) {
                ASSERT_BSONOBJ_EQ(queryCursor->nextSafe(), BSON("a" << i));
            }
            // Having exhausted the two results returned by the initial find, we set the
            // batchSize to 1 and issue a single getMore via DBClientCursor::more().
            // Because the 'exhaust' flag is set, the server will generate a series of
            // internal getMores and stream them back to the client until the cursor is
            // exhausted, without the client sending any further getMore requests. We
            // expect this request to hang at the
            // 'waitWithPinnedCursorDuringGetMoreBatch' failpoint.
            queryCursor->setBatchSize(1);
            if (findCmd.getTailable() && findCmd.getAwaitData()) {
                queryCursor->setAwaitDataTimeoutMS(awaitDataTimeoutMS);
                if (lastKnownCommittedOpTime) {
                    auto term = lastKnownCommittedOpTime.value().getTerm();
                    queryCursor->setCurrentTermAndLastCommittedOpTime(term,
                                                                      lastKnownCommittedOpTime);
                }
            }
            ASSERT(queryCursor->more());
        });

    // Wait until the parallel operation initializes its cursor.
    {
        stdx::unique_lock<Latch> lk(cursorIdMutex);
        cursorIdCV.wait_for(lk, parallelWaitTimeoutMS, [&] { return cursorId.has_value(); });
    }
    ASSERT(cursorId);
    LOGV2(20607,
          "Started exhaust query with cursorId: {queryCursor_getCursorId}",
          "queryCursor_getCursorId"_attr = *cursorId);
    queryCursorId = *cursorId;
    return queryThread;
}

void runOneGetMore(DBClientBase* conn, CursorId queryCursorId, int nDocsReturned) {
    const auto curOpMatch = BSON("command.collection" << testNSS.coll() << "command.getMore"
                                                      << queryCursorId << "failpointMsg"
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
        BSON("command.getMore" << queryCursorId << "failpointMsg"
                               << "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch")));

    // Re-enable the original failpoint to catch the next getMore, and release the current one.
    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn, true);
    setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn, false);
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
    CursorId queryCursorId;

    // Execute a query on a separate thread, with the 'exhaust' flag set.
    auto queryThread = startExhaustQuery(queryConnection.get(), queryCursorId);
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
        runOneGetMore(conn.get(), queryCursorId, i);
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
    queryConnection->connect(connStr.getServers()[0], testBackgroundAppName, boost::none);
    CursorId queryCursorId;

    // Execute a query on a separate thread, with the 'exhaust' flag set.
    auto queryThread = startExhaustQuery(queryConnection.get(), queryCursorId);
    // Ensure that, regardless of whether the test completes or fails, we release all failpoints.
    ON_BLOCK_EXIT([&conn, &queryThread] {
        setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), false);
        setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn.get(), false);
        queryThread.wait();
    });

    // This will allow the initial getMore to run.
    runOneGetMore(conn.get(), queryCursorId, 2);

    // The next getMore will be an exhaust getMore. Confirm that the exhaust getMore appears in the
    // $currentOp output.
    auto curOpMatch = BSON("command.collection" << testNSS.coll() << "command.getMore"
                                                << queryCursorId << "failpointMsg"
                                                << "waitWithPinnedCursorDuringGetMoreBatch"
                                                << "cursor.nDocsReturned" << 3);
    ASSERT(confirmCurrentOpContents(conn.get(), curOpMatch));

    if (disconnectAfterGetMoreBatch) {
        // Allow the exhaust getMore to run but block it before sending out the response.
        setWaitAfterCommandFinishesExecutionFailpoint(conn.get(), true);
        setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), false);
        ASSERT(confirmCurrentOpContents(conn.get(),
                                        BSON("command.getMore"
                                             << queryCursorId << "failpointMsg"
                                             << "waitAfterCommandFinishesExecution")));
    }

    // Kill the client connection while the exhaust getMore is blocked on the failpoint.
    queryConnection->shutdownAndDisallowReconnect();
    LOGV2(20608, "Killed exhaust connection.");

    if (disconnectAfterGetMoreBatch) {
        // Disable the failpoint to allow the exhaust getMore to continue sending out the response
        // after the client disconnects. This will result in a broken pipe error.
        setWaitAfterCommandFinishesExecutionFailpoint(conn.get(), false);
    }

    curOpMatch = BSON("command.collection" << testNSS.coll() << "command.getMore" << queryCursorId);
    // Confirm that the exhaust getMore was interrupted and does not appear in the $currentOp
    // output.
    const bool expectEmptyResult = true;
    ASSERT(confirmCurrentOpContents(conn.get(), curOpMatch, expectEmptyResult));

    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn.get(), false);
    setWaitBeforeUnpinningOrDeletingCursorAfterGetMoreBatchFailpoint(conn.get(), false);

    curOpMatch = BSON("type"
                      << "idleCursor"
                      << "cursor.cursorId" << queryCursorId);
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

TEST(CurrentOpExhaustCursorTest, ExhaustCursorUpdatesLastKnownCommittedOpTime) {
    auto fixtureConn = connect();

    // We need to test the lastKnownCommittedOpTime in exhaust getMore requests. So we need a
    // replica set.
    if (!fixtureConn->isReplicaSetMember()) {
        return;
    }

    // Connect directly to the primary.
    DBClientBase* conn = &static_cast<DBClientReplicaSet*>(fixtureConn.get())->primaryConn();
    ASSERT(conn);

    conn->dropCollection(testNSS);
    // Create a capped collection to run tailable awaitData queries on.
    conn->createCollection(
        testNSS, 1024 /* size of collection */, true /* capped */, 10 /* max number of objects */);
    // Insert initial records into the capped collection.
    for (int i = 0; i < 5; i++) {
        auto insertCmd =
            BSON("insert" << testNSS.coll() << "documents" << BSON_ARRAY(BSON("a" << i)));
        auto reply = conn->runCommand(OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired, testNSS.dbName(), insertCmd));
        ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
    }

    // Get the lastAppliedOpTime after the initial inserts.
    auto lastAppliedOpTime = getLastAppliedOpTime(conn);

    // Create a new connection to the primary for the exhaust query.
    const auto fixtureQueryConn = connect(testBackgroundAppName);
    DBClientBase* queryConn =
        &static_cast<DBClientReplicaSet*>(fixtureQueryConn.get())->primaryConn();
    CursorId queryCursorId;

    // Enable a failpoint to block getMore during execution to avoid races between getCursorId() and
    // receiving new batches.
    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn, true);

    // Initiate a tailable awaitData exhaust cursor with lastKnownCommittedOpTime being the
    // lastAppliedOpTime.
    auto queryThread = startExhaustQuery(queryConn,
                                         queryCursorId,
                                         QueryOption_CursorTailable | QueryOption_AwaitData,
                                         Milliseconds(1000),  // awaitData timeout
                                         lastAppliedOpTime);  // lastKnownCommittedOpTime

    // Assert non-zero cursorId.
    ASSERT_NE(queryCursorId, 0LL);

    // Disable failpoint and allow exhaust queries to run.
    setWaitWithPinnedCursorDuringGetMoreBatchFailpoint(conn, false);

    ON_BLOCK_EXIT([&conn, &queryThread] { queryThread.wait(); });

    // Test that the cursor's lastKnownCommittedOpTime is eventually advanced to the
    // lastAppliedOpTime.
    auto curOpMatch =
        BSON("command.collection" << testNSS.coll() << "command.getMore" << queryCursorId
                                  << "cursor.lastKnownCommittedOpTime" << lastAppliedOpTime);
    ASSERT(confirmCurrentOpContents(conn, curOpMatch));

    // Inserting more records to unblock awaitData and advance the commit point.
    for (int i = 5; i < 8; i++) {
        auto insertCmd =
            BSON("insert" << testNSS.coll() << "documents" << BSON_ARRAY(BSON("a" << i)));
        auto reply = conn->runCommand(OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired, testNSS.dbName(), insertCmd));
        ASSERT_OK(getStatusFromCommandResult(reply->getCommandReply()));
    }

    // Get the new lastAppliedOpTime after the inserts.
    lastAppliedOpTime = getLastAppliedOpTime(conn);

    // Test that the cursor's lastKnownCommittedOpTime is eventually advanced to the
    // new lastAppliedOpTime.
    curOpMatch =
        BSON("command.collection" << testNSS.coll() << "command.getMore" << queryCursorId
                                  << "cursor.lastKnownCommittedOpTime" << lastAppliedOpTime);
    ASSERT(confirmCurrentOpContents(conn, curOpMatch));
}
}  // namespace mongo
