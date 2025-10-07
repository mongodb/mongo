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
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/query/exec/async_results_merger.h"
#include "mongo/s/query/exec/next_high_watermark_determining_strategy.h"
#include "mongo/s/query/exec/results_merger_test_fixture.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

namespace mongo {

using AsyncResultsMergerTest = ResultsMergerTestFixture;

namespace {

LogicalSessionId parseSessionIdFromCmd(BSONObj cmdObj) {
    return LogicalSessionId::parse(IDLParserContext("lsid"), cmdObj["lsid"].Obj());
}

BSONObj makeResumeToken(Timestamp clusterTime, UUID uuid, BSONObj docKey) {
    ResumeTokenData data(clusterTime,
                         ResumeTokenData::kDefaultTokenVersion,
                         /* txnOpIndex */ 0,
                         uuid,
                         /* eventIdentifier */ Value(Document{docKey}));
    return ResumeToken(data).toDocument().toBson();
}

BSONObj makeHighWaterMarkToken(Timestamp ts) {
    return BSON("_id" << AsyncResultsMergerTest::makePostBatchResumeToken(ts));
}

// Returns true if the high matermark token t1 is greater or equal compared to the high watermark
// token t0.
bool isMonotonicallyIncreasing(const BSONObj& highWaterMarkTokenT0,
                               const BSONObj& highWaterMarkTokenT1) {
    return AsyncResultsMerger::checkHighWaterMarkIsMonotonicallyIncreasing(
        highWaterMarkTokenT0, highWaterMarkTokenT1, change_stream_constants::kSortSpec);
}

// Returns true if the high matermark token for timestamp t1 is greater or equal compared to the
// high watermark token for timestamp t0.
bool isMonotonicallyIncreasing(Timestamp timestampT0, Timestamp timestampT1) {
    return isMonotonicallyIncreasing(makeHighWaterMarkToken(timestampT0),
                                     makeHighWaterMarkToken(timestampT1));
}

NextHighWaterMarkDeterminingStrategyPtr buildNextHighWaterMarkDeterminingStrategy(
    bool recognizeControlEvents) {
    return NextHighWaterMarkDeterminingStrategyFactory::createForChangeStream(
        AsyncResultsMergerTest::buildARMParamsForChangeStream(), recognizeControlEvents);
}

TEST_F(AsyncResultsMergerTest, ResponseReceivedWhileDetachedFromOperationContext) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);

    arm->detachFromOperationContext();
    ASSERT_EQ(0, arm->numberOfBufferedRemoteResponses_forTest());

    scheduleNetworkResponses(std::move(responses));

    // As the ARM is detached from the OperationContext, it cannot immediately process the recceived
    // response. IT must instead buffer it, so it can process it later when it is attached to an
    // OperationContext again.
    ASSERT_EQ(1, arm->numberOfBufferedRemoteResponses_forTest());

    arm->reattachToOperationContext(operationContext());

    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());

    ASSERT_EQ(1, arm->numberOfBufferedRemoteResponses_forTest());

    // ARM returns the correct results.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());

    // Buffered remote response must have been processes as part of 'nextReady()' call.
    ASSERT_EQ(0, arm->numberOfBufferedRemoteResponses_forTest());

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());

    // Number of buffered remote responses must remain at zero.
    ASSERT_EQ(0, arm->numberOfBufferedRemoteResponses_forTest());

    // After returning all the buffered results, ARM returns EOF immediately because the cursor was
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, SingleShardUnsorted) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // Now that the responses have been delivered, ARM is ready to return results.
    ASSERT_TRUE(arm->ready());

    // Because the response contained a cursorId of 0, ARM marked the remote as exhausted.
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns the correct results.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, ARM returns EOF immediately because the cursor was
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, SingleShardSorted) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{$sortKey: [5]}"), fromjson("{$sortKey: [6]}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // Now that the responses have been delivered, ARM is ready to return results.
    ASSERT_TRUE(arm->ready());

    // Because the response contained a cursorId of 0, ARM marked the remote as exhausted.
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns all results in order.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [5]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [6]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, ARM returns EOF immediately because the cursor was
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, MultiShardUnsorted) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses));

    // ARM is ready to return first result.
    ASSERT_TRUE(arm->ready());

    // ARM is not exhausted, because second shard has yet to respond.
    ASSERT_FALSE(arm->remotesExhausted());

    // ARM returns results from first shard immediately.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());

    // There are no further buffered results, so ARM is not ready.
    ASSERT_FALSE(arm->ready());

    // Make next event to be signaled.
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {
        fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses));

    // ARM is ready to return remaining results.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns results from second shard immediately.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, the ARM returns EOF immediately because both shards
    // cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, MultiShardSorted) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: [5]}"), fromjson("{$sortKey: [6]}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses));

    // ARM is not ready to return results until receiving responses from all remotes.
    ASSERT_FALSE(arm->ready());

    // ARM is not exhausted, because second shard has yet to respond.
    ASSERT_FALSE(arm->remotesExhausted());

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: [3]}"), fromjson("{$sortKey: [9]}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses));

    // Now that all remotes have responded, ARM is ready to return results.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns all results in sorted order.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [3]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [5]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [6]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [9]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, the ARM returns EOF immediately because both shards
    // cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}


TEST_F(AsyncResultsMergerTest, MultiShardUnsortedShardReceivesErrorBetweenReadyAndNextReady) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Schedule request.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(5), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(6), batch2);

    scheduleNetworkResponses(std::move(responses));

    // ARM returns results from first shard immediately.
    executor()->waitForEvent(readyEvent);

    // ARM is ready to return first results.
    ASSERT_TRUE(arm->ready());

    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());

    // Call ready(), but do not consume the next ready event yet.
    ASSERT_TRUE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Make another shard return an error response. This sets this remote's status to an error
    // internally.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::BadValue, "bad thing happened")));
    executor()->waitForEvent(readyEvent);

    // Fetching the next event should fail with that error, instead of causing invariant failures or
    // tasserts.
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(statusWithNext.getStatus().reason(), "bad thing happened");

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, SingleShardUnsortedCloseAfterReceivingAllResults) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));
    ASSERT_EQ(1, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_FALSE(arm->ready());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};

    // Respond with a cursorId of 0, meaning the remote cursor is already closed.
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns the correct result.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_FALSE(networkHasReadyRequests());

    // Remove the only shard.
    arm->closeShardCursors({kTestShardIds[0]});

    // No killCursors command is supposed to be executed here, as the remote cursor was already
    // closed before.
    ASSERT_FALSE(networkHasReadyRequests());

    ASSERT_EQ(0, arm->getNumRemotes());
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST_F(AsyncResultsMergerTest, MultipleShardsUnsortedCloseWhileRequestsInFlight) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));

    auto arm = makeARMFromExistingCursors(std::move(cursors));
    ASSERT_EQ(2, arm->getNumRemotes());
    ASSERT_FALSE(arm->ready());

    // Shared_ptr should be used exactly once.
    ASSERT_EQ(1, arm.use_count());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // This will send two getMore requests, so the use_count for the shared_ptr will increase to 3.
    arm->scheduleGetMores().ignore();

    ASSERT_EQ(3, arm.use_count());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};

    // Respond with two batches.
    responses.emplace_back(kTestNss, CursorId(5), batch);
    responses.emplace_back(kTestNss, CursorId(6), batch);
    scheduleNetworkResponses(std::move(responses));

    // Wait until all pending requests have been processed.
    for (; getNumPendingRequests() > 0;) {
    }

    // All pending network responses have been processed, so the use_count should be down to 1
    // again.
    ASSERT_EQ(1, arm.use_count());

    // Discard all outstanding unconsumed results.
    arm->kill(operationContext());
}

TEST_F(AsyncResultsMergerTest, SingleShardUnsortedCloseWithMoreResultsPending) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));
    ASSERT_EQ(1, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_FALSE(arm->ready());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};

    // Respond with a non-zero cursorId, meaning the remote cursor remains open.
    responses.emplace_back(kTestNss, CursorId(5), batch);

    scheduleNetworkResponses(std::move(responses));

    ASSERT_TRUE(arm->ready());

    // ARM returns the correct result.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());

    arm->closeShardCursors({kTestShardIds[0]});

    // We expect to see one killCursors command call for the remote cursor.
    ASSERT_TRUE(networkHasReadyRequests());
    ASSERT_EQ(1, getNumPendingRequests());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 5);
    blackHoleNextRequest();

    ASSERT_EQ(0, arm->getNumRemotes());
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST_F(AsyncResultsMergerTest, MultiShardSortedClose) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);
    ASSERT_EQ(2, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[1]));
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: [5]}"), fromjson("{$sortKey: [6]}")};
    responses.emplace_back(kTestNss, CursorId(5), batch1);
    scheduleNetworkResponses(std::move(responses));

    // ARM is not ready to return results until receiving responses from all remotes.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: [3]}"), fromjson("{$sortKey: [9]}")};

    // Respond with a cursorId of 0, meaning the remote cursor is closed.
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses));

    // ARM returns all results in sorted order.
    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [3]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [5]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [6]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // First shard has only responded up to '6', we cannot move forward to '9' already because the
    // first shard may return further values between '6' and '9' in the future.
    ASSERT_FALSE(arm->ready());

    // Close shard 1. This drops the document '{$sortKey: [9]}' from the result.
    arm->closeShardCursors({kTestShardIds[1]});

    // No killCursors command is supposed to be executed here, as the remote cursor was already
    // closed.
    ASSERT_FALSE(networkHasReadyRequests());

    ASSERT_EQ(1, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[1]));

    readyEvent = unittest::assertGet(arm->nextEvent());

    // First shard responds with more data.
    responses.clear();
    std::vector<BSONObj> batch3 = {
        fromjson("{$sortKey: [7]}"), fromjson("{$sortKey: [11]}"), fromjson("{$sortKey: [12]}")};

    // Respond with a cursorId of 5, meaning the remote cursor remains open.
    responses.emplace_back(kTestNss, CursorId(5), batch3);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());

    // We can now process all responses. We should not see the not-yet consumed document from the
    // shard that we already closed.
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [7]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [11]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // Close shard 0.
    arm->closeShardCursors({kTestShardIds[0]});

    // We expect to see one killCursors command call for the remote cursor.
    ASSERT_TRUE(networkHasReadyRequests());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 5);
    blackHoleNextRequest();

    ASSERT_EQ(0, arm->getNumRemotes());
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[0]));

    // Now everything is fully consumed.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST_F(AsyncResultsMergerTest, MultiShardSortedCloseAndReopen) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);
    ASSERT_EQ(2, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[1]));
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // First responses from the shards; the handleBatchResponse callback is run and ARM's remote
    // gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: [1]}")};
    responses.emplace_back(kTestNss, CursorId(5), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: [2]}")};
    responses.emplace_back(kTestNss, CursorId(6), batch2);

    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [1]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // Close shard 1. This removes the document '{$sortKey: [2]}' from the result.
    arm->closeShardCursors({kTestShardIds[1]});

    // We expect to see one killCursors command call for the remote cursor.
    ASSERT_TRUE(networkHasReadyRequests());
    ASSERT_EQ(1, getNumPendingRequests());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 6);
    blackHoleNextRequest();

    ASSERT_EQ(1, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[1]));

    ASSERT_FALSE(arm->ready());

    readyEvent = unittest::assertGet(arm->nextEvent());

    // First shard responds with more data.
    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{$sortKey: [3]}")};
    responses.emplace_back(kTestNss, CursorId(5), batch3);

    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [3]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // Re-add shard 1
    cursors.clear();
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 7, {})));
    arm->addNewShardCursors(std::move(cursors));

    ASSERT_EQ(2, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[1]));

    // After returning all the buffered results, the ARM returns EOF immediately because both shards
    // cursors were exhausted.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    readyEvent = unittest::assertGet(arm->nextEvent());

    // Shards respond with more data.
    responses.clear();
    std::vector<BSONObj> batch4 = {fromjson("{$sortKey: [4]}"), fromjson("{$sortKey: [6]}")};
    responses.emplace_back(kTestNss, CursorId(5), batch4);
    std::vector<BSONObj> batch5 = {fromjson("{$sortKey: [5]}"), fromjson("{$sortKey: [6]}")};
    responses.emplace_back(kTestNss, CursorId(7), batch5);

    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());

    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [4]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [5]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [6]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    arm->closeShardCursors({kTestShardIds[0], kTestShardIds[1]});

    // We expect to see two killCursors command calls for the remote cursors.
    ASSERT_EQ(2, getNumPendingRequests());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 5);
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(1u).cmdObj, 7);
    blackHoleNextRequest();
    blackHoleNextRequest();

    ASSERT_EQ(0, arm->getNumRemotes());
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[1]));
}

TEST_F(AsyncResultsMergerTest, CloseCursorWithUnconsumedInitialBatch) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<BSONObj> batch = {BSON("$sortKey" << BSON_ARRAY(23)),
                                  BSON("$sortKey" << BSON_ARRAY(42))};

    std::vector<RemoteCursor> cursors;
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 42, batch)));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);
    ASSERT_EQ(1, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Close cursor immediately, without consuming any results. This drops the documents '{$sortKey:
    // [23]}' and '{$sortKey: [42]}' from the result.
    arm->closeShardCursors({kTestShardIds[0]});
    ASSERT_EQ(0, arm->getNumRemotes());

    // We expect to see one killCursors command call for the remote cursor.
    ASSERT_TRUE(networkHasReadyRequests());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 42);
    blackHoleNextRequest();

    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());

    // Re-add the same shard, but with different documents.
    batch = {BSON("$sortKey" << BSON_ARRAY(123)), BSON("$sortKey" << BSON_ARRAY(456))};

    cursors.clear();
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 99, batch)));
    arm->addNewShardCursors(std::move(cursors));

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());
    ASSERT_BSONOBJ_EQ(BSON("$sortKey" << BSON_ARRAY(123)),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_BSONOBJ_EQ(BSON("$sortKey" << BSON_ARRAY(456)),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // Close cursor again.
    arm->closeShardCursors({kTestShardIds[0]});
    ASSERT_EQ(0, arm->getNumRemotes());

    // We expect to see one killCursors command call for the remote cursor.
    ASSERT_TRUE(networkHasReadyRequests());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 99);
    blackHoleNextRequest();

    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST_F(AsyncResultsMergerTest, MultiShardSortedCloseWhileWaitingForShardResult) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 23, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 42, {})));

    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);
    ASSERT_EQ(2, arm->getNumRemotes());
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[1]));
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    unittest::assertGet(arm->nextEvent());

    // One schedule a response from one of the shards.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {BSON("$sortKey" << BSON_ARRAY(1)),
                                  BSON("$sortKey" << BSON_ARRAY(10))};
    responses.emplace_back(kTestNss, CursorId(23), batch);

    scheduleNetworkResponses(std::move(responses));

    // Results are not ready because only one of the shards has responded.
    ASSERT_FALSE(arm->ready());

    // Close the cursor for the shard for which we have not scheduled a response yet. That
    // immediately makes results available.
    arm->closeShardCursors({kTestShardIds[1]});

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    ASSERT_TRUE(arm->hasCursorForShard_forTest(kTestShardIds[0]));
    ASSERT_FALSE(arm->hasCursorForShard_forTest(kTestShardIds[1]));
    ASSERT_EQ(1, arm->getNumRemotes());

    // We expect to see one killCursors command call for the remote cursor.
    ASSERT_TRUE(networkHasReadyRequests());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 42);
    blackHoleNextRequest();

    ASSERT_BSONOBJ_EQ(BSON("$sortKey" << BSON_ARRAY(1)),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(BSON("$sortKey" << BSON_ARRAY(10)),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());
}

TEST_F(AsyncResultsMergerTest, MultiShardMultipleGets) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(5), batch1);
    scheduleNetworkResponses(std::move(responses));

    // ARM is ready to return first result.
    ASSERT_TRUE(arm->ready());

    // ARM is not exhausted, because second shard has yet to respond and first shard's response did
    // not contain cursorId=0.
    ASSERT_FALSE(arm->remotesExhausted());

    // ARM returns results from first shard immediately.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());

    // There are no further buffered results, so ARM is not ready.
    ASSERT_FALSE(arm->ready());

    // Make next event to be signaled.
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {
        fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses));

    // ARM is ready to return second shard's results.
    ASSERT_TRUE(arm->ready());

    // ARM is not exhausted, because first shard's response did not contain cursorId=0.
    ASSERT_FALSE(arm->remotesExhausted());

    // ARM returns results from second shard immediately.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()).getResult());

    // ARM is not ready to return results until further results are obtained from first shard.
    ASSERT_FALSE(arm->ready());

    // Make next event to be signaled.
    readyEvent = unittest::assertGet(arm->nextEvent());

    // First shard returns remainder of results.
    responses.clear();
    std::vector<BSONObj> batch3 = {
        fromjson("{_id: 7}"), fromjson("{_id: 8}"), fromjson("{_id: 9}")};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));

    // ARM is ready to return remaining results.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns remaining results immediately.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 7}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 8}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 9}"), *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, the ARM returns EOF immediately because both shards
    // cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, HighWaterMarkTestChangeStreamV1) {
    runHighWaterMarkTest(false /* recognizeControlEvents */);
}

TEST_F(AsyncResultsMergerTest, HighWaterMarkTestRecognizeControlEvents) {
    runHighWaterMarkTest(true /* recognizeControlEvents */);
}

DEATH_TEST_REGEX_F(AsyncResultsMergerTest,
                   SetInitialHighWaterMarkWithTimeGoingBackwards,
                   "Tripwire assertion.*10359104") {
    AsyncResultsMergerParams params = AsyncResultsMergerTest::buildARMParamsForChangeStream();
    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    // No high water mark set initially.
    ASSERT_BSONOBJ_EQ(BSONObj(), arm->getHighWaterMark());

    // Inject a high water mark.
    auto highWaterMark = makePostBatchResumeToken(Timestamp(42, 33));

    arm->setInitialHighWaterMark(highWaterMark);
    ASSERT_BSONOBJ_EQ(highWaterMark, arm->getHighWaterMark());

    auto highWaterMarkBefore = makePostBatchResumeToken(Timestamp(42, 32));
    ASSERT_THROWS_CODE(
        arm->setInitialHighWaterMark(highWaterMarkBefore), AssertionException, 10359104);
}

TEST_F(AsyncResultsMergerTest, HandleControlEventsWithUniqueTimestamps) {
    AsyncResultsMergerParams params = AsyncResultsMergerTest::buildARMParamsForChangeStream();

    std::vector<RemoteCursor> cursors;
    auto initialPBRT = makePostBatchResumeToken(Timestamp(23, 23));
    cursors.push_back(makeRemoteCursor(kTestShardIds[0],
                                       kTestShardHosts[0],
                                       CursorResponse(kTestNss, 1, {}, boost::none, initialPBRT)));
    params.setRemotes(std::move(cursors));

    auto arm = buildARM(std::move(params), true /* recognizeControlEvents */);

    const std::vector<BSONObj> pbrts = {
        makePostBatchResumeToken(Timestamp(42, 1)),
        makePostBatchResumeToken(Timestamp(42, 2)),
        makePostBatchResumeToken(Timestamp(42, 3)),
        makePostBatchResumeToken(Timestamp(42, 4)),
        makePostBatchResumeToken(Timestamp(42, 5)),
        makePostBatchResumeToken(Timestamp(42, 6)),
        makePostBatchResumeToken(Timestamp(42, 7)),
    };

    // All events following have unique timestamps.
    const std::vector<BSONObj> batch = {
        BSON("_id" << pbrts[0] << "$sortKey" << BSON_ARRAY(pbrts[0]) << "value" << 1),
        BSON("_id" << pbrts[1] << "$sortKey" << BSON_ARRAY(pbrts[1]) << "value" << 2),
        BSON("_id" << pbrts[2] << "$sortKey" << BSON_ARRAY(pbrts[2])
                   << Document::metaFieldChangeStreamControlEvent << 1 << "value"
                   << "control1"),
        BSON("_id" << pbrts[3] << "$sortKey" << BSON_ARRAY(pbrts[3]) << "value" << 3),
        BSON("_id" << pbrts[4] << "$sortKey" << BSON_ARRAY(pbrts[4])
                   << Document::metaFieldChangeStreamControlEvent << 1 << "value"
                   << "control2"),
        BSON("_id" << pbrts[5] << "$sortKey" << BSON_ARRAY(pbrts[5]) << "value" << 4),
        BSON("_id" << pbrts[6] << "$sortKey" << BSON_ARRAY(pbrts[6]) << "value" << 5),
    };

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Deliver response.
    std::vector<CursorResponse> responses;
    responses.emplace_back(kTestNss, CursorId(1), batch, boost::none, pbrts.back());

    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    // Still expect the initial high water mark.
    ASSERT_BSONOBJ_EQ(initialPBRT, arm->getHighWaterMark());

    for (std::size_t i = 0; i < batch.size(); ++i) {
        ASSERT_TRUE(arm->ready());
        ASSERT_BSONOBJ_EQ(batch[i], *unittest::assertGet(arm->nextReady()).getResult());
        ASSERT_BSONOBJ_EQ(pbrts[i], arm->getHighWaterMark());
    }

    ASSERT_FALSE(arm->ready());
    ASSERT_BSONOBJ_EQ(pbrts.back(), arm->getHighWaterMark());
}

TEST_F(AsyncResultsMergerTest, HandleControlEventsWithNonUniqueTimestamps) {
    AsyncResultsMergerParams params = AsyncResultsMergerTest::buildARMParamsForChangeStream();

    std::vector<RemoteCursor> cursors;
    auto initialPBRT = makePostBatchResumeToken(Timestamp(23, 23));
    cursors.push_back(makeRemoteCursor(kTestShardIds[0],
                                       kTestShardHosts[0],
                                       CursorResponse(kTestNss, 1, {}, boost::none, initialPBRT)));
    params.setRemotes(std::move(cursors));

    auto arm = buildARM(std::move(params), true /* recognizeControlEvents */);

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // The followings events do not have unique timestamps.
    const std::vector<BSONObj> pbrts = {
        makePostBatchResumeToken(Timestamp(42, 1)),
        makePostBatchResumeToken(Timestamp(42, 2)),
        makePostBatchResumeToken(Timestamp(42, 2)),
        makePostBatchResumeToken(Timestamp(42, 3)),
        makePostBatchResumeToken(Timestamp(42, 3)),
        makePostBatchResumeToken(Timestamp(42, 4)),
        makePostBatchResumeToken(Timestamp(42, 5)),
        makePostBatchResumeToken(Timestamp(42, 5)),
        makePostBatchResumeToken(Timestamp(42, 5)),
    };

    // The control events following have timestamps identical to another event.
    const std::vector<BSONObj> batch = {
        BSON("_id" << pbrts[0] << "$sortKey" << BSON_ARRAY(pbrts[0]) << "value" << 1),
        BSON("_id" << pbrts[1] << "$sortKey" << BSON_ARRAY(pbrts[1]) << "value" << 2),
        BSON("_id" << pbrts[2] << "$sortKey" << BSON_ARRAY(pbrts[2])
                   << Document::metaFieldChangeStreamControlEvent << 1 << "value"
                   << "control1"),
        BSON("_id" << pbrts[3] << "$sortKey" << BSON_ARRAY(pbrts[3]) << "value" << 3),
        BSON("_id" << pbrts[4] << "$sortKey" << BSON_ARRAY(pbrts[4])
                   << Document::metaFieldChangeStreamControlEvent << 1 << "value"
                   << "control2"),
        BSON("_id" << pbrts[5] << "$sortKey" << BSON_ARRAY(pbrts[5]) << "value" << 4),
        BSON("_id" << pbrts[6] << "$sortKey" << BSON_ARRAY(pbrts[6]) << "value" << 5),
        BSON("_id" << pbrts[7] << "$sortKey" << BSON_ARRAY(pbrts[7])
                   << Document::metaFieldChangeStreamControlEvent << 1 << "value" << "control3"),
        BSON("_id" << pbrts[8] << "$sortKey" << BSON_ARRAY(pbrts[8])
                   << Document::metaFieldChangeStreamControlEvent << 1 << "value" << "control4"),
    };

    // Deliver response.
    std::vector<CursorResponse> responses;
    responses.emplace_back(kTestNss, CursorId(1), batch, boost::none, pbrts.back());

    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    // Still expect the initial high water mark.
    ASSERT_BSONOBJ_EQ(initialPBRT, arm->getHighWaterMark());

    ASSERT_TRUE(arm->ready());

    for (std::size_t i = 0; i < batch.size(); ++i) {
        ASSERT_TRUE(arm->ready());
        ASSERT_BSONOBJ_EQ(batch[i], *unittest::assertGet(arm->nextReady()).getResult());
        ASSERT_BSONOBJ_EQ(pbrts[i], arm->getHighWaterMark());
    }

    ASSERT_FALSE(arm->ready());
    ASSERT_BSONOBJ_EQ(pbrts.back(), arm->getHighWaterMark());
}

DEATH_TEST_REGEX_F(AsyncResultsMergerTest, MakePBRTGoBackInTime, "Tripwire assertion.*10359104") {
    AsyncResultsMergerParams params = AsyncResultsMergerTest::buildARMParamsForChangeStream();

    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    params.setRemotes(std::move(cursors));

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Deliver response.
    std::vector<CursorResponse> responses;
    auto pbrtLow = makePostBatchResumeToken(Timestamp(42, 1));
    auto pbrtHigh = makePostBatchResumeToken(Timestamp(42, 2));

    std::vector<BSONObj> batch1 = {
        BSON("_id" << pbrtHigh << "$sortKey" << BSON_ARRAY(pbrtHigh) << "value" << 1),
    };
    responses.emplace_back(kTestNss, CursorId(1), batch1, boost::none, pbrtHigh);

    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());

    ASSERT_BSONOBJ_EQ(BSON("_id" << pbrtHigh << "$sortKey" << BSON_ARRAY(pbrtHigh) << "value" << 1),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_BSONOBJ_EQ(pbrtHigh, arm->getHighWaterMark());

    // Intentionally schedule a response with a lower PBRT. This will crash the ARM.
    readyEvent = unittest::assertGet(arm->nextEvent());

    std::vector<BSONObj> batch2 = {
        BSON("_id" << pbrtLow << "$sortKey" << BSON_ARRAY(pbrtLow) << "value" << 1),
    };
    responses.clear();
    responses.emplace_back(kTestNss, CursorId(1), batch2, boost::none, pbrtLow);

    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_THROWS_CODE(
        *unittest::assertGet(arm->nextReady()).getResult(), AssertionException, 10359104);
}

TEST_F(AsyncResultsMergerTest, CompoundSortKey) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {a: -1, b: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 6, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 7, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    // Schedule requests.
    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Deliver responses.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: [5, 9]}"), fromjson("{$sortKey: [4, 20]}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: [10, 11]}"),
                                   fromjson("{$sortKey: [4, 4]}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{$sortKey: [10, 12]}"),
                                   fromjson("{$sortKey: [5, 9]}")};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    // ARM returns all results in sorted order.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [10, 11]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [10, 12]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [5, 9]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [5, 9]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [4, 4]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: [4, 20]}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, the ARM returns EOF immediately because both shards
    // cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, SortedButNoSortKey) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {a: -1, b: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Parsing the batch results in an error because the sort key is missing.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{a: 2, b: 1}"), fromjson("{a: 1, b: 2}")};
    responses.emplace_back(kTestNss, CursorId(1), batch1);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::InternalError);

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, HasFirstBatch) {
    std::vector<BSONObj> firstBatch = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    std::vector<RemoteCursor> cursors;
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, std::move(firstBatch))));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Because there was firstBatch, ARM is immediately ready to return results.
    ASSERT_TRUE(arm->ready());

    // Because the cursorId was not zero, ARM is not exhausted.
    ASSERT_FALSE(arm->remotesExhausted());

    // ARM returns the correct results.
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());

    // Now that the firstBatch results have been returned, ARM must wait for further results.
    ASSERT_FALSE(arm->ready());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // Now that the responses have been delivered, ARM is ready to return results.
    ASSERT_TRUE(arm->ready());

    // Because the response contained a cursorId of 0, ARM marked the remote as exhausted.
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns the correct results.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, ARM returns EOF immediately because the cursor was
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, OneShardHasInitialBatchOtherShardExhausted) {
    std::vector<BSONObj> firstBatch = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    std::vector<RemoteCursor> cursors;
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, std::move(firstBatch))));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 0, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Because there was firstBatch, ARM is immediately ready to return results.
    ASSERT_TRUE(arm->ready());

    // Because one of the remotes' cursorId was not zero, ARM is not exhausted.
    ASSERT_FALSE(arm->remotesExhausted());

    // ARM returns the correct results.
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());

    // Now that the firstBatch results have been returned, ARM must wait for further results.
    ASSERT_FALSE(arm->ready());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // Now that the responses have been delivered, ARM is ready to return results.
    ASSERT_TRUE(arm->ready());

    // Because the response contained a cursorId of 0, ARM marked the remote as exhausted.
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns the correct results.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, ARM returns EOF immediately because the cursor was
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, StreamResultsFromOneShardIfOtherDoesntRespond) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Both shards respond with the first batch.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(1), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(kTestNss, CursorId(2), batch2);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // When we ask the shards for their next batch, the first shard responds and the second shard
    // never responds.
    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(kTestNss, CursorId(1), batch3);
    scheduleNetworkResponses(std::move(responses));
    blackHoleNextRequest();
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // We can continue to return results from first shard, while second shard remains unresponsive.
    responses.clear();
    std::vector<BSONObj> batch4 = {fromjson("{_id: 7}"), fromjson("{_id: 8}")};
    responses.emplace_back(kTestNss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 7}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 8}"), *unittest::assertGet(arm->nextReady()).getResult());

    auto killFuture = arm->kill(operationContext());
    shutdownExecutor();
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, ErrorOnMismatchedCursorIds) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(kTestNss, CursorId(456), batch);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT(!arm->nextReady().isOK());

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, BadResponseReceivedFromShard) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 456, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 789, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    BSONObj response1 = CursorResponse(kTestNss, CursorId(123), batch1)
                            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    BSONObj response2 = fromjson("{foo: 'bar'}");
    std::vector<BSONObj> batch3 = {fromjson("{_id: 4}"), fromjson("{_id: 5}")};
    BSONObj response3 = CursorResponse(kTestNss, CursorId(789), batch3)
                            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    scheduleNetworkResponseObjs({response1, response2, response3});
    runReadyCallbacks();
    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, ErrorReceivedFromShard) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 3, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(1), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(kTestNss, CursorId(2), batch2);
    scheduleNetworkResponses(std::move(responses));

    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::BadValue, "bad thing happened")));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(statusWithNext.getStatus().reason(), "bad thing happened");

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, ErrorCantScheduleEventBeforeLastSignaled) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Error to call nextEvent()() before the previous event is signaled.
    ASSERT_NOT_OK(arm->nextEvent().getStatus());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, NextEventAfterTaskExecutorShutdown) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    shutdownExecutor();
    ASSERT_EQ(ErrorCodes::ShutdownInProgress, arm->nextEvent().getStatus());
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillAfterTaskExecutorShutdownWithOutstandingBatches) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Make a request to the shard that will never get answered.
    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());
    blackHoleNextRequest();

    // Executor shuts down before a response is received.
    shutdownExecutor();
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();

    // Ensure that the executor finishes all of the outstanding callbacks before the ARM is freed.
    executor()->join();
}

TEST_F(AsyncResultsMergerTest, KillNoBatchesRequested) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto killFuture = arm->kill(operationContext());
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    // Killed cursors are considered ready, but return an error when you try to receive the next
    // doc.
    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());

    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillAllRemotesExhausted) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 3, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));

    auto killFuture = arm->kill(operationContext());

    // ARM shouldn't schedule killCursors on anything since all of the remotes are exhausted.
    ASSERT_FALSE(networkHasReadyRequests());

    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillNonExhaustedCursorWithoutPendingRequest) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    // Cursor 3 is not exhausted.
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(kTestNss, CursorId(123), batch3);
    scheduleNetworkResponses(std::move(responses));

    auto killFuture = arm->kill(operationContext());

    // ARM should schedule killCursors on cursor 123
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 123);

    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillTwoOutstandingBatches) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 3, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses));

    // Kill event will only be signalled once the callbacks for the pending batches have run.
    auto killFuture = arm->kill(operationContext());

    // Check that the ARM kills both batches.
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 2);
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(1u).cmdObj, 3);

    // Run the callbacks which were canceled.
    runReadyCallbacks();

    // Ensure that we properly signal those waiting for more results to be ready.
    executor()->waitForEvent(readyEvent);
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, NextEventErrorsAfterKill) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(1), batch1);
    scheduleNetworkResponses(std::move(responses));

    auto killFuture = arm->kill(operationContext());

    // Attempting to schedule more network operations on a killed arm is an error.
    ASSERT_NOT_OK(arm->nextEvent().getStatus());

    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillCalledTwice) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));
    auto killFuture1 = arm->kill(operationContext());
    auto killFuture2 = arm->kill(operationContext());
    killFuture1.wait();
    killFuture2.wait();
}

TEST_F(AsyncResultsMergerTest, KillCursorCmdHasNoTimeout) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    auto* opCtx = operationContext();
    opCtx->setDeadlineAfterNowBy(Microseconds::zero(), ErrorCodes::MaxTimeMSExpired);
    auto killFuture = arm->kill(opCtx);
    ASSERT_EQ(executor::RemoteCommandRequest::kNoTimeout, getNthPendingRequest(0u).timeout);
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillBeforeNext) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Mark the OperationContext as killed from this thread.
    {
        stdx::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->markKilled(ErrorCodes::Interrupted);
    }

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto nextStatus = arm->nextEvent();
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::Interrupted);
    });

    // Wait for the merger to be interrupted.
    future.default_timed_get();

    // Kill should complete.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillBeforeNextWithTwoRemotes) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Mark the OperationContext as killed from this thread.
    {
        stdx::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->markKilled(ErrorCodes::Interrupted);
    }

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto nextStatus = arm->nextEvent();
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::Interrupted);
    });

    // Wait for the merger to be interrupted.
    future.default_timed_get();

    // Kill should complete.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, TailableBasic) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());

    // In the tailable case, we expect EOF after every batch.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_FALSE(arm->remotesExhausted());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(123), batch2);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_FALSE(arm->remotesExhausted());

    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, TailableEmptyBatch) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Remote responds with an empty batch and a non-zero cursor id.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch;
    responses.emplace_back(kTestNss, CursorId(123), batch);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    // After receiving an empty batch, the ARM should return boost::none, but remotes should not be
    // marked as exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_FALSE(arm->remotesExhausted());

    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, TailableExhaustedCursor) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Remote responds with an empty batch and a zero cursor id.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch;
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    // Afterwards, the ARM should return boost::none and remote cursors should be marked as
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST_F(AsyncResultsMergerTest, GetMoreBatchSizes) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 3}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(1), batch1);

    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());

    responses.clear();

    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    readyEvent = unittest::assertGet(arm->nextEvent());

    BSONObj scheduledCmd = getNthPendingRequest(0).cmdObj;
    auto cmd = GetMoreCommandRequest::parse(
        IDLParserContext{"getMore"},
        scheduledCmd.addField(BSON("$db" << "anydbname").firstElement()));
    ASSERT_EQ(*cmd.getBatchSize(), 3LL);
    ASSERT_EQ(cmd.getCommandParameter(), 1LL);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, AllowPartialResults) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 97, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 98, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 99, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    // An error occurs with the first host.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::AuthenticationFailed, "authentication failed")));
    ASSERT_FALSE(arm->ready());

    // Instead of propagating the error, we should be willing to return results from the two
    // remaining shards.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(98), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(99), batch2);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_TRUE(arm->partialResultsReturned());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Now the second host becomes unreachable. We should still be willing to return results from
    // the third shard.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::AuthenticationFailed, "authentication failed")));
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(99), batch3);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->partialResultsReturned());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Once the last reachable shard indicates that its cursor is closed, we're done.
    responses.clear();
    std::vector<BSONObj> batch4 = {};
    responses.emplace_back(kTestNss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, AllowPartialResultsSingleNode) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 98, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(98), batch);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    // The lone host involved in this query returns an error. This should simply cause us to return
    // EOF.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::AuthenticationFailed, "authentication failed")));
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->partialResultsReturned());
}

TEST_F(AsyncResultsMergerTest, AllowPartialResultsSingleNodeExchangePassthroughError) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 98, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(98), batch);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    // The lone host involved in this query returns an error. This should simply cause us to return
    // EOF. Note that using the special error code 'ExchangePassthrough' drains the cursor, but does
    // not mark the remote as having returned partial results!
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::ExchangePassthrough, "exchange passthrough error")));

    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_FALSE(arm->partialResultsReturned());
}

TEST_F(AsyncResultsMergerTest, AllowPartialResultsOnRetriableErrorNoRetries) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 2, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    // First host returns single result
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // From the second host we get a network (retriable) error.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::HostUnreachable, "host unreachable")));

    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());

    ASSERT_TRUE(arm->partialResultsReturned());

    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
}

TEST_F(AsyncResultsMergerTest, MaxTimeMSExpiredAllowPartialResultsTrue) {
    BSONObj findCmd = BSON("find" << "testcoll"
                                  << "allowPartialResults" << true);
    std::vector<RemoteCursor> cursors;
    // Two shards.
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    // First host returns single result.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // From the second host we get a MaxTimeMSExpired error.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::MaxTimeMSExpired, "MaxTimeMSExpired")));

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->partialResultsReturned());
    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, MaxTimeMSExpiredAllowPartialResultsFalse) {
    BSONObj findCmd = BSON("find" << "testcoll"
                                  << "allowPartialResults" << false);
    std::vector<RemoteCursor> cursors;
    // two shards
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // First host returns single result
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // From the second host we get a MaxTimeMSExpired error.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::MaxTimeMSExpired, "MaxTimeMSExpired")));

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::MaxTimeMSExpired);
    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, AllowPartialResultsOnMaxTimeMSExpiredThenLateData) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<RemoteCursor> cursors;
    // two shards
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 2, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // From the first host we get a MaxTimeMSExpired error.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::MaxTimeMSExpired, "MaxTimeMSExpired")));

    // Second host returns single result *after* first host times out.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->partialResultsReturned());
    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
}

TEST_F(AsyncResultsMergerTest, ReturnsErrorOnRetriableError) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2], kTestShardHosts[2], CursorResponse(kTestNss, 2, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    ASSERT_FALSE(arm->partialResultsReturned());

    // Both hosts return network (retriable) errors.
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::HostUnreachable, "host unreachable")));
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::HostUnreachable, "host unreachable")));

    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());

    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::HostUnreachable);
    ASSERT_EQ(statusWithNext.getStatus().reason(), "host unreachable");

    ASSERT_FALSE(arm->partialResultsReturned());

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, GetMoreCommandRequestIncludesMaxTimeMS) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true, awaitData: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());

    readyEvent = unittest::assertGet(arm->nextEvent());

    ASSERT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));

    // Pending getMore request should already have been scheduled without the maxTimeMS.
    BSONObj expectedCmdObj = BSON("getMore" << CursorId(123) << "collection"
                                            << "testcoll");
    ASSERT_BSONOBJ_EQ(getNthPendingRequest(0).cmdObj, expectedCmdObj);

    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(kTestNss, CursorId(123), batch2);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());

    readyEvent = unittest::assertGet(arm->nextEvent());

    // The next getMore request should include the maxTimeMS.
    expectedCmdObj = BSON("getMore" << CursorId(123) << "collection"
                                    << "testcoll"
                                    << "maxTimeMS" << 789);
    ASSERT_BSONOBJ_EQ(getNthPendingRequest(0).cmdObj, expectedCmdObj);

    // Clean up.
    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));
}

DEATH_TEST_REGEX_F(
    AsyncResultsMergerTest,
    SortedTailableInvariantsIfInitialBatchHasNoPostBatchResumeToken,
    R"#(Invariant failure.*_promisedMinSortKeys.empty\(\) || _promisedMinSortKeys.size\(\) == _remotes.size\(\))#") {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    // Create one cursor whose initial response has a postBatchResumeToken.
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: [{_data: '"
                      << firstDocSortKey.firstElement().String() << "'}]}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(kTestNss, 123, {firstCursorResponse}, boost::none, pbrtFirstCursor)));
    // Create a second cursor whose initial batch has no PBRT.
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 456, {})));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // We should be dead by now.
    MONGO_UNREACHABLE;
}

DEATH_TEST_REGEX_F(AsyncResultsMergerTest,
                   SortedTailableCursorInvariantsIfOneOrMoreRemotesHasEmptyPostBatchResumeToken,
                   R"#(Invariant failure.*!postBatchResumeToken->isEmpty\(\))#") {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    BSONObj pbrtFirstCursor;
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: [{_data: '"
                      << firstDocSortKey.firstElement().String() << "'}]}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(kTestNss, 123, {firstCursorResponse}, boost::none, pbrtFirstCursor)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_TRUE(arm->ready());

    // We should be dead by now.
    MONGO_UNREACHABLE;
}

TEST_F(AsyncResultsMergerTest, SortedTailableCursorNotReadyIfRemoteHasLowerPostBatchResumeToken) {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: [{data: '"
                      << firstDocSortKey.firstElement().String() << "'}]}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(kTestNss, 123, {firstCursorResponse}, boost::none, pbrtFirstCursor)));
    auto tooLowPBRT = makePostBatchResumeToken(Timestamp(1, 2));
    cursors.push_back(makeRemoteCursor(kTestShardIds[1],
                                       kTestShardHosts[1],
                                       CursorResponse(kTestNss, 456, {}, boost::none, tooLowPBRT)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    ASSERT_FALSE(arm->ready());

    // Clean up the cursors.
    std::vector<CursorResponse> responses;
    responses.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{});
    scheduleNetworkResponses(std::move(responses));
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, SortedTailableCursorNewShardOrderedAfterExisting) {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    ASSERT_FALSE(arm->ready());

    // Schedule one response with an oplog timestamp in it.
    std::vector<CursorResponse> responses;
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 6));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: [{_data: '"
                      << firstDocSortKey.firstElement().String() << "'}]}");
    std::vector<BSONObj> batch1 = {firstCursorResponse};
    auto firstDoc = batch1.front();
    responses.emplace_back(kTestNss, CursorId(123), batch1, boost::none, pbrtFirstCursor);
    scheduleNetworkResponses(std::move(responses));

    // Should be ready now.
    ASSERT_TRUE(arm->ready());

    // Add the new shard.
    auto tooLowPBRT = makePostBatchResumeToken(Timestamp(1, 3));
    std::vector<RemoteCursor> newCursors;
    newCursors.push_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(kTestNss, 456, {}, boost::none, tooLowPBRT)));
    arm->addNewShardCursors(std::move(newCursors));

    // Now shouldn't be ready, our guarantee from the new shard isn't sufficiently advanced.
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Schedule another response from the other shard.
    responses.clear();
    auto secondDocSortKey = makeResumeToken(Timestamp(1, 5), uuid, BSON("_id" << 2));
    auto pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 6));
    auto secondCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 5)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 2}}, $sortKey: [{_data: '"
                      << secondDocSortKey.firstElement().String() << "'}]}");
    std::vector<BSONObj> batch2 = {secondCursorResponse};
    auto secondDoc = batch2.front();
    responses.emplace_back(kTestNss, CursorId(456), batch2, boost::none, pbrtSecondCursor);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(firstCursorResponse, *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(secondCursorResponse, *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());

    readyEvent = unittest::assertGet(arm->nextEvent());

    // Clean up the cursors.
    responses.clear();
    std::vector<BSONObj> batch3 = {};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));
    responses.clear();
    std::vector<BSONObj> batch4 = {};
    responses.emplace_back(kTestNss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses));
}

TEST_F(AsyncResultsMergerTest, SortedTailableCursorNewShardOrderedBeforeExisting) {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    ASSERT_FALSE(arm->ready());

    // Schedule one response with an oplog timestamp in it.
    std::vector<CursorResponse> responses;
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: [{_data: '"
                      << firstDocSortKey.firstElement().String() << "'}]}");
    std::vector<BSONObj> batch1 = {firstCursorResponse};
    responses.emplace_back(kTestNss, CursorId(123), batch1, boost::none, pbrtFirstCursor);
    scheduleNetworkResponses(std::move(responses));

    // Should be ready now.
    ASSERT_TRUE(arm->ready());

    // Add the new shard.
    auto tooLowPBRT = makePostBatchResumeToken(Timestamp(1, 3));
    std::vector<RemoteCursor> newCursors;
    newCursors.push_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(kTestNss, 456, {}, boost::none, tooLowPBRT)));
    arm->addNewShardCursors(std::move(newCursors));

    // Now shouldn't be ready, our guarantee from the new shard isn't sufficiently advanced.
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Schedule another response from the other shard.
    responses.clear();
    auto secondDocSortKey = makeResumeToken(Timestamp(1, 3), uuid, BSON("_id" << 2));
    auto pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 5));
    auto secondCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 3)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 2}}, $sortKey: [{_data: '"
                      << secondDocSortKey.firstElement().String() << "'}]}");
    std::vector<BSONObj> batch2 = {secondCursorResponse};
    // The last observed time should still be later than the first shard, so we can get the data
    // from it.
    responses.emplace_back(kTestNss, CursorId(456), batch2, boost::none, pbrtSecondCursor);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(secondCursorResponse, *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(firstCursorResponse, *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());

    readyEvent = unittest::assertGet(arm->nextEvent());

    // Clean up the cursors.
    responses.clear();
    std::vector<BSONObj> batch3 = {};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));
    responses.clear();
    std::vector<BSONObj> batch4 = {};
    responses.emplace_back(kTestNss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses));
}

TEST_F(AsyncResultsMergerTest, SortedTailableCursorReturnsHighWaterMarkSortKey) {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    std::vector<RemoteCursor> cursors;
    // Create three cursors with empty initial batches. Each batch has a PBRT.
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0],
                         kTestShardHosts[0],
                         CursorResponse(kTestNss, 123, {}, boost::none, pbrtFirstCursor)));
    auto pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 1));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(kTestNss, 456, {}, boost::none, pbrtSecondCursor)));
    auto pbrtThirdCursor = makePostBatchResumeToken(Timestamp(1, 4));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[2],
                         kTestShardHosts[2],
                         CursorResponse(kTestNss, 789, {}, boost::none, pbrtThirdCursor)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    // We have no results to return, so the ARM is not ready.
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // The high water mark should be the second cursor's PBRT, since it is the lowest of the three.
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtSecondCursor);

    // Advance the PBRT of the second cursor. It should still be the lowest. The fixture expects
    // each cursor to be updated in-order, so we keep the first and third PBRTs constant.
    pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 3));
    std::vector<BSONObj> emptyBatch = {};
    scheduleNetworkResponse({kTestNss, CursorId(123), emptyBatch, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse({kTestNss, CursorId(456), emptyBatch, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse({kTestNss, CursorId(789), emptyBatch, boost::none, pbrtThirdCursor});
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtSecondCursor);
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Advance the second cursor again, so that it surpasses the other two. The third cursor becomes
    // the new high water mark.
    pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 6));
    scheduleNetworkResponse({kTestNss, CursorId(123), emptyBatch, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse({kTestNss, CursorId(456), emptyBatch, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse({kTestNss, CursorId(789), emptyBatch, boost::none, pbrtThirdCursor});
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtThirdCursor);
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Advance the third cursor such that the first cursor becomes the high water mark.
    pbrtThirdCursor = makePostBatchResumeToken(Timestamp(1, 7));
    scheduleNetworkResponse({kTestNss, CursorId(123), emptyBatch, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse({kTestNss, CursorId(456), emptyBatch, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse({kTestNss, CursorId(789), emptyBatch, boost::none, pbrtThirdCursor});
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtFirstCursor);
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Clean up the cursors.
    std::vector<BSONObj> cleanupBatch = {};
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
}

TEST_F(AsyncResultsMergerTest, SortedTailableCursorDoesNotAdvanceHighWaterMarkForIneligibleCursor) {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    std::vector<RemoteCursor> cursors;
    // Create three cursors with empty initial batches. Each batch has a PBRT. The third cursor is
    // the $changeStream opened on "config.shards" to monitor for the addition of new shards.
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0],
                         kTestShardHosts[0],
                         CursorResponse(kTestNss, 123, {}, boost::none, pbrtFirstCursor)));
    auto pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 3));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(kTestNss, 456, {}, boost::none, pbrtSecondCursor)));
    auto pbrtConfigCursor = makePostBatchResumeToken(Timestamp(1, 1));
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[2],
        kTestShardHosts[2],
        CursorResponse(
            NamespaceString::kConfigsvrShardsNamespace, 789, {}, boost::none, pbrtConfigCursor)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    // We have no results to return, so the ARM is not ready.
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // For a change stream cursor on "config.shards", the first batch is not eligible to provide the
    // HWM. Despite the fact that 'pbrtConfigCursor' has the lowest PBRT, the ARM returns the PBRT
    // of 'pbrtSecondCursor' as the current high water mark. This guards against the possibility
    // that the user requests a start time in the future. The "config.shards" cursor must start
    // monitoring for shards at the current point in time, and so its initial PBRT will be lower
    // than that of the shards. We do not wish to return a high water mark to the client that is
    // earlier than the start time they specified in their request.
    auto initialHighWaterMark = arm->getHighWaterMark();
    ASSERT_BSONOBJ_EQ(initialHighWaterMark, pbrtSecondCursor);

    // Advance the PBRT of the 'pbrtConfigCursor'. It is still the lowest, but is ineligible to
    // provide the high water mark because it is still lower than the high water mark that was
    // already returned. As above, the guards against the possibility that the user requested a
    // stream with a start point at an arbitrary point in the future.
    pbrtConfigCursor = makePostBatchResumeToken(Timestamp(1, 2));
    scheduleNetworkResponse({kTestNss, CursorId(123), {}, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse({kTestNss, CursorId(456), {}, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse({NamespaceString::kConfigsvrShardsNamespace,
                             CursorId(789),
                             {},
                             boost::none,
                             pbrtConfigCursor});

    // The high water mark has not advanced from its previous value.
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), initialHighWaterMark);
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // If the "config.shards" cursor returns a result, this document does not advance the HWM. We
    // consume this event internally but do not return it to the client, and its resume token is not
    // actually resumable. We therefore do not want to expose it to the client via the PBRT.
    const auto configUUID = UUID::gen();
    auto configEvent = fromjson("{_id: 'shard_add_event'}");
    pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 15));
    pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 13));
    pbrtConfigCursor = makeResumeToken(Timestamp(1, 11), configUUID, configEvent);
    configEvent =
        configEvent.addField(BSON("$sortKey" << BSON_ARRAY(pbrtConfigCursor)).firstElement());
    scheduleNetworkResponse({kTestNss, CursorId(123), {}, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse({kTestNss, CursorId(456), {}, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse({NamespaceString::kConfigsvrShardsNamespace,
                             CursorId(789),
                             {configEvent},
                             boost::none,
                             pbrtConfigCursor});

    // The config cursor has a lower sort key than the other shards, so we can retrieve the event.
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(configEvent, *unittest::assertGet(arm->nextReady()).getResult());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Reading the config cursor event document does not advance the high water mark.
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), initialHighWaterMark);
    ASSERT_FALSE(arm->ready());

    // If the next config batch is empty but the PBRT is still the resume token of the addShard
    // event, it does not advance the ARM's high water mark sort key.
    scheduleNetworkResponse({kTestNss, CursorId(123), {}, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse({kTestNss, CursorId(456), {}, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse({NamespaceString::kConfigsvrShardsNamespace,
                             CursorId(789),
                             {},
                             boost::none,
                             pbrtConfigCursor});
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), initialHighWaterMark);
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // If none of the above criteria obtain, then the "config.shards" cursor is eligible to advance
    // the ARM's high water mark. The only reason we allow the config.shards cursor to participate
    // in advancing of the high water mark at all is so that we cannot end up in a situation where
    // the config cursor is always the lowest and the high water mark can therefore never advance.
    pbrtConfigCursor = makePostBatchResumeToken(Timestamp(1, 12));
    scheduleNetworkResponse({kTestNss, CursorId(123), {}, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse({kTestNss, CursorId(456), {}, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse({NamespaceString::kConfigsvrShardsNamespace,
                             CursorId(789),
                             {},
                             boost::none,
                             pbrtConfigCursor});
    ASSERT_BSONOBJ_GT(arm->getHighWaterMark(), initialHighWaterMark);
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtConfigCursor);
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());

    // Clean up the cursors.
    std::vector<BSONObj> cleanupBatch = {};
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
}

TEST_F(AsyncResultsMergerTest, GetMoreCommandRequestWithoutTailableCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, GetMoreCommandRequestWithoutAwaitDataCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, ShardCanErrorInBetweenReadyAndNextEvent) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    scheduleErrorResponse(executor::RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::BadValue, "bad thing happened")));

    ASSERT_EQ(ErrorCodes::BadValue, arm->nextEvent().getStatus());

    // Required to kill the 'arm' on error before destruction.
    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, KillShouldNotWaitForRemoteCommandsBeforeSchedulingKillCursors) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Kill the ARM while a batch is still outstanding. The callback for the outstanding batch
    // should be canceled.
    auto killFuture = arm->kill(operationContext());

    // Check that the ARM will run killCursors.
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    // Let the callback run now that it's been canceled.
    runReadyCallbacks();

    executor()->waitForEvent(readyEvent);
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, GetMoresShouldNotIncludeLSIDOrTxnNumberIfNoneSpecified) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // There should be no lsid txnNumber in the scheduled getMore.
    ASSERT_OK(arm->nextEvent().getStatus());
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);

        ASSERT(request.cmdObj["lsid"].eoo());
        ASSERT(request.cmdObj["txnNumber"].eoo());

        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });
}

TEST_F(AsyncResultsMergerTest, GetMoresShouldIncludeLSIDIfSpecified) {
    auto lsid = makeLogicalSessionIdForTest();
    operationContext()->setLogicalSessionId(lsid);

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // There should be an lsid and no txnNumber in the scheduled getMore.
    ASSERT_OK(arm->nextEvent().getStatus());
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);

        ASSERT_EQ(parseSessionIdFromCmd(request.cmdObj), lsid);
        ASSERT(request.cmdObj["txnNumber"].eoo());

        return CursorResponse(kTestNss, 1LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    // Subsequent requests still pass the lsid.
    ASSERT(arm->ready());
    ASSERT_OK(arm->nextReady().getStatus());
    ASSERT_FALSE(arm->ready());

    ASSERT_OK(arm->nextEvent().getStatus());
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);

        ASSERT_EQ(parseSessionIdFromCmd(request.cmdObj), lsid);
        ASSERT(request.cmdObj["txnNumber"].eoo());

        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });
}

TEST_F(AsyncResultsMergerTest, GetMoresShouldIncludeLSIDAndTxnNumIfSpecified) {
    auto lsid = makeLogicalSessionIdForTest();
    operationContext()->setLogicalSessionId(lsid);

    const TxnNumber txnNumber = 5;
    operationContext()->setTxnNumber(txnNumber);

    operationContext()->setInMultiDocumentTransaction();

    RouterOperationContextSession session(operationContext());
    TransactionRouter::get(operationContext())
        .beginOrContinueTxn(
            operationContext(), txnNumber, TransactionRouter::TransactionActions::kStart);
    TransactionRouter::get(operationContext()).setDefaultAtClusterTime(operationContext());

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // The first scheduled getMore should pass the txnNumber the ARM was constructed with.
    ASSERT_OK(arm->nextEvent().getStatus());
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);

        ASSERT_EQ(parseSessionIdFromCmd(request.cmdObj), lsid);
        ASSERT_EQ(request.cmdObj["txnNumber"].numberLong(), txnNumber);

        BSONObjBuilder bob{CursorResponse(kTestNss, 1LL, {BSON("x" << 1)})
                               .toBSON(CursorResponse::ResponseType::SubsequentResponse)};
        bob.appendBool("readOnly", true);
        return bob.obj();
    });

    // Subsequent requests still pass the txnNumber.
    ASSERT(arm->ready());
    ASSERT_OK(arm->nextReady().getStatus());
    ASSERT_FALSE(arm->ready());

    // Subsequent getMore requests should include txnNumber.
    ASSERT_OK(arm->nextEvent().getStatus());
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);

        ASSERT_EQ(parseSessionIdFromCmd(request.cmdObj), lsid);
        ASSERT_EQ(request.cmdObj["txnNumber"].numberLong(), txnNumber);
        BSONObjBuilder bob{CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
                               .toBSON(CursorResponse::ResponseType::SubsequentResponse)};
        bob.appendBool("readOnly", true);
        return bob.obj();
    });
}

TEST_F(AsyncResultsMergerTest, ProcessAdditionalParticipants) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagAllowAdditionalParticipants", true);

    auto lsid = makeLogicalSessionIdForTest();
    operationContext()->setLogicalSessionId(lsid);

    const TxnNumber txnNumber = 5;
    operationContext()->setTxnNumber(txnNumber);

    operationContext()->setInMultiDocumentTransaction();

    RouterOperationContextSession session(operationContext());
    TransactionRouter::get(operationContext())
        .beginOrContinueTxn(
            operationContext(), txnNumber, TransactionRouter::TransactionActions::kStart);
    TransactionRouter::get(operationContext()).setDefaultAtClusterTime(operationContext());

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // The first scheduled getMore should pass the txnNumber the ARM was constructed with.
    ASSERT_OK(arm->nextEvent().getStatus());
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);

        ASSERT_EQ(parseSessionIdFromCmd(request.cmdObj), lsid);
        ASSERT_EQ(request.cmdObj["txnNumber"].numberLong(), txnNumber);

        BSONObjBuilder bob{CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
                               .toBSON(CursorResponse::ResponseType::SubsequentResponse)};
        bob.appendBool("readOnly", true);
        std::vector<BSONObj> additionalParticipants = {
            BSON("shardId" << kTestShardIds[1] << "readOnly" << true)};
        bob.appendElements(
            BSON(TxnResponseMetadata::kAdditionalParticipantsFieldName << additionalParticipants));
        return bob.obj();
    });

    // Process responses.
    ASSERT(arm->ready());
    ASSERT_OK(arm->nextReady().getStatus());

    auto addedShard = TransactionRouter::get(operationContext()).getParticipant(kTestShardIds[1]);
    ASSERT(addedShard);
    ASSERT_EQ(addedShard->readOnly, TransactionRouter::Participant::ReadOnly::kReadOnly);
}

TEST_F(AsyncResultsMergerTest, ProcessAdditionalParticipantsEvenIfKilled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagAllowAdditionalParticipants", true);

    auto lsid = makeLogicalSessionIdForTest();
    operationContext()->setLogicalSessionId(lsid);

    const TxnNumber txnNumber = 5;
    operationContext()->setTxnNumber(txnNumber);

    operationContext()->setInMultiDocumentTransaction();

    RouterOperationContextSession session(operationContext());
    TransactionRouter::get(operationContext())
        .beginOrContinueTxn(
            operationContext(), txnNumber, TransactionRouter::TransactionActions::kStart);
    TransactionRouter::get(operationContext()).setDefaultAtClusterTime(operationContext());

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // The first scheduled getMore should pass the txnNumber the ARM was constructed with.
    ASSERT_OK(arm->nextEvent().getStatus());
    arm->detachFromOperationContext();
    // The reply comes after we've sent the request.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);

        ASSERT_EQ(parseSessionIdFromCmd(request.cmdObj), lsid);
        ASSERT_EQ(request.cmdObj["txnNumber"].numberLong(), txnNumber);

        BSONObjBuilder bob{CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
                               .toBSON(CursorResponse::ResponseType::SubsequentResponse)};
        bob.appendBool("readOnly", true);
        std::vector<BSONObj> additionalParticipants = {
            BSON("shardId" << kTestShardIds[1] << "readOnly" << true)};
        bob.appendElements(
            BSON(TxnResponseMetadata::kAdditionalParticipantsFieldName << additionalParticipants));
        return bob.obj();
    });

    // Proceed to kill the ARM. We haven't yet processed the additional participants.
    auto addedShard = TransactionRouter::get(operationContext()).getParticipant(kTestShardIds[1]);
    ASSERT_FALSE(addedShard);
    arm->reattachToOperationContext(operationContext());
    arm->kill(operationContext());
    // We now have killed the ARM. Additional participants should be processed.
    addedShard = TransactionRouter::get(operationContext()).getParticipant(kTestShardIds[1]);
    ASSERT(addedShard);
    ASSERT_EQ(addedShard->readOnly, TransactionRouter::Participant::ReadOnly::kReadOnly);
}

DEATH_TEST_F(AsyncResultsMergerTest,
             ShouldFailIfAskedToPerformGetMoresWithoutAnOpCtx,
             "Cannot schedule a getMore without an OperationContext") {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true, awaitData: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    arm->detachFromOperationContext();
    arm->scheduleGetMores().ignore();  // Should crash.
}

TEST_F(AsyncResultsMergerTest, ShouldNotScheduleGetMoresWithoutAnOperationContext) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true, awaitData: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // While detached from the OperationContext, schedule an empty batch response. Because the
    // response is empty and this is a tailable cursor, the ARM will need to run another getMore on
    // that host, but it should not schedule this without a non-null OperationContext.
    arm->detachFromOperationContext();
    {
        std::vector<CursorResponse> responses;
        std::vector<BSONObj> emptyBatch;
        responses.emplace_back(kTestNss, CursorId(123), emptyBatch);
        scheduleNetworkResponses(std::move(responses));
    }

    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(networkHasReadyRequests());  // Tests that we haven't asked for the next batch yet.

    // After manually requesting the next getMore, the ARM should be ready.
    arm->reattachToOperationContext(operationContext());
    ASSERT_OK(arm->scheduleGetMores());

    // Schedule the next getMore response.
    {
        std::vector<CursorResponse> responses;
        std::vector<BSONObj> nonEmptyBatch = {fromjson("{_id: 1}")};
        responses.emplace_back(kTestNss, CursorId(123), nonEmptyBatch);
        scheduleNetworkResponses(std::move(responses));
    }

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    auto killFuture = arm->kill(operationContext());
    killFuture.wait();
}

TEST_F(AsyncResultsMergerTest, IncludeQueryStatsMetricsIncludedInGetMore) {
    auto runGetMore = [this](bool requestParams) {
        BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
        std::vector<RemoteCursor> cursors;
        cursors.push_back(makeRemoteCursor(
            kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));

        auto params = makeARMParamsFromExistingCursors(std::move(cursors), findCmd);
        params.setRequestQueryStatsFromRemotes(requestParams);

        auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

        // Schedule the request for the getMore.
        auto readyEvent = unittest::assertGet(arm->nextEvent());

        // Stash the request so we can inspect it.
        auto cmd = getNthPendingRequest(0u).cmdObj;

        // Schedule a response.
        std::vector<CursorResponse> responses;
        std::vector<BSONObj> nonEmptyBatch = {fromjson("{_id: 1}")};
        responses.emplace_back(kTestNss, CursorId(0), nonEmptyBatch);
        scheduleNetworkResponses(std::move(responses));

        // Kill the ARM.
        auto killFuture = arm->kill(operationContext());
        killFuture.wait();

        return cmd;
    };

    {
        // The original query was not selected for query stats - we don't expect to see the flag.
        auto cmd = runGetMore(false);
        ASSERT_TRUE(cmd["includeQueryStatsMetrics"].eoo());
    }

    {
        // The original query was selected for query stats - we expect to see the flag true.
        auto cmd = runGetMore(true);
        ASSERT_TRUE(cmd["includeQueryStatsMetrics"].Bool());
    }
}

TEST_F(AsyncResultsMergerTest, RemoteMetricsAggregatedLocally) {
    auto scheduleResponse = [&](CursorId id, std::vector<BSONObj> batch, CursorMetrics metrics) {
        std::vector<CursorResponse> responses;
        responses.emplace_back(kTestNss,
                               id,
                               std::move(batch),
                               boost::none /* atClusterTime */,
                               boost::none /* postBatchResumeToken */,
                               boost::none /* writeConcernError */,
                               boost::none /* varsField */,
                               boost::none /* explainField*/,
                               boost::none /* cursorType */,
                               std::move(metrics));
        scheduleNetworkResponses(std::move(responses));
    };

    CursorId id(123);

    AsyncResultsMergerParams params;
    params.setNss(kTestNss);

    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, id, {})));
    params.setRemotes(std::move(cursors));

    auto arm = buildARM(std::move(params), false /* recognizeControlEvents */);

    // Schedule the request for a getMore.
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Schedule a response.
    {
        CursorMetrics metrics(2 /* keysExamined */,
                              5 /* docsExamined */,
                              13 /* bytesRead */,
                              17 /* readingTimeMicros */,
                              7 /* workingTimeMillis */,
                              false /* hasSortStage */,
                              true /* usedDisk */,
                              true /* fromMultiPlanner */,
                              true /* fromPlanCache */,
                              37 /*cpuNanos */
        );
        metrics.setDelinquentAcquisitions(3);
        metrics.setTotalAcquisitionDelinquencyMillis(100);
        metrics.setMaxAcquisitionDelinquencyMillis(80);
        metrics.setNumInterruptChecks(3);
        scheduleResponse(id, {fromjson("{_id: 1}")}, std::move(metrics));
    }

    // Wait for the batch to be processed and read the single object.
    executor()->waitForEvent(readyEvent);
    ASSERT_FALSE(unittest::assertGet(arm->nextReady()).isEOF());

    // Schedule the request for a second getMore.
    readyEvent = unittest::assertGet(arm->nextEvent());

    {
        auto remoteMetrics = arm->peekMetrics_forTest();
        ASSERT_EQ(remoteMetrics.keysExamined, 2);
        ASSERT_EQ(remoteMetrics.docsExamined, 5);
        ASSERT_EQ(remoteMetrics.bytesRead, 13);
        ASSERT_EQ(remoteMetrics.readingTime, Microseconds(17));
        ASSERT_EQ(remoteMetrics.clusterWorkingTime, Milliseconds(7));
        ASSERT_EQ(remoteMetrics.hasSortStage, false);
        ASSERT_EQ(remoteMetrics.usedDisk, true);
        ASSERT_EQ(remoteMetrics.fromMultiPlanner, true);
        ASSERT_EQ(remoteMetrics.fromPlanCache, true);
        ASSERT_EQ(remoteMetrics.cpuNanos, Nanoseconds(37));
        ASSERT_EQ(remoteMetrics.delinquentAcquisitions, 3);
        ASSERT_EQ(remoteMetrics.totalAcquisitionDelinquencyMillis, Milliseconds(100));
        ASSERT_EQ(remoteMetrics.maxAcquisitionDelinquencyMillis, Milliseconds(80));
        ASSERT_EQ(remoteMetrics.numInterruptChecks, 3);
    }

    // Schedule a second response.
    {
        CursorMetrics metrics(7 /* keysExamined */,
                              11 /* docsExamined */,
                              17 /* bytesRead */,
                              19 /* readingTimeMicros */,
                              13 /* workingTimeMillis */,
                              false /* hasSortStage */,
                              true /* usedDisk */,
                              true /* fromMultiPlanner */,
                              false /* fromPlanCache */,
                              121 /*cpuNanos */
        );
        metrics.setDelinquentAcquisitions(2);
        metrics.setTotalAcquisitionDelinquencyMillis(150);
        metrics.setMaxAcquisitionDelinquencyMillis(120);
        metrics.setNumInterruptChecks(2);
        scheduleResponse(CursorId(0), {fromjson("{_id: 2}")}, std::move(metrics));
    }

    // Wait for the final batch to be processed and read the object.
    executor()->waitForEvent(readyEvent);
    ASSERT_FALSE(unittest::assertGet(arm->nextReady()).isEOF());

    {
        // Metrics aggregated, result should be the sum of the responses.
        auto remoteMetrics = arm->takeMetrics();
        ASSERT_EQ(remoteMetrics.keysExamined, 9);
        ASSERT_EQ(remoteMetrics.docsExamined, 16);
        ASSERT_EQ(remoteMetrics.bytesRead, 30);
        ASSERT_EQ(remoteMetrics.readingTime, Microseconds(36));
        ASSERT_EQ(remoteMetrics.clusterWorkingTime, Milliseconds(20));
        ASSERT_EQ(remoteMetrics.hasSortStage, false);
        ASSERT_EQ(remoteMetrics.usedDisk, true);
        ASSERT_EQ(remoteMetrics.fromMultiPlanner, true);
        ASSERT_EQ(remoteMetrics.fromPlanCache, false);
        ASSERT_EQ(remoteMetrics.cpuNanos, Nanoseconds(158));
        ASSERT_EQ(remoteMetrics.delinquentAcquisitions, 5);
        ASSERT_EQ(remoteMetrics.totalAcquisitionDelinquencyMillis, Milliseconds(250));
        ASSERT_EQ(remoteMetrics.maxAcquisitionDelinquencyMillis, Milliseconds(120));
        ASSERT_EQ(remoteMetrics.numInterruptChecks, 5);
    }

    {
        // Metrics should have been reset to a zero state.
        auto remoteMetrics = arm->peekMetrics_forTest();
        ASSERT_EQ(remoteMetrics.keysExamined, 0);
        ASSERT_EQ(remoteMetrics.docsExamined, 0);
        ASSERT_EQ(remoteMetrics.clusterWorkingTime, Milliseconds(0));
        ASSERT_EQ(remoteMetrics.hasSortStage, false);
        ASSERT_EQ(remoteMetrics.usedDisk, false);
        ASSERT_EQ(remoteMetrics.fromMultiPlanner, false);
        ASSERT_EQ(remoteMetrics.fromPlanCache, true);
        ASSERT_EQ(remoteMetrics.cpuNanos, Nanoseconds(0));
        ASSERT_EQ(remoteMetrics.delinquentAcquisitions, 0);
        ASSERT_EQ(remoteMetrics.totalAcquisitionDelinquencyMillis, Milliseconds(0));
        ASSERT_EQ(remoteMetrics.maxAcquisitionDelinquencyMillis, Milliseconds(0));
        ASSERT_EQ(remoteMetrics.numInterruptChecks, 0);
    }

    // Read the EOF
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, CanAccessParams) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 5, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Check actual parameters.
    ASSERT_EQ(kTestNss, arm->params().getNss());
    ASSERT_EQ(1, arm->params().getRemotes().size());

    // Schedule requests. We need to do this because the dtor of AsyncResultsMerger fires an
    // assertion if the remotes are not exhausted and the AsyncResultsMerger hasn't been killed.
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // Now the AsyncResultsMerger can go out of scope without triggering the assertion failure.
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST(AsyncResultsMergerTest, CheckHighWaterMarkTokensAreMonotonicallyIncreasing) {
    // Compare high water mark tokens against each other.
    ASSERT_FALSE(isMonotonicallyIncreasing(Timestamp(42, 1), Timestamp(42, 0)));
    ASSERT_FALSE(isMonotonicallyIncreasing(Timestamp(42, 1), Timestamp(41, 0)));
    ASSERT_FALSE(isMonotonicallyIncreasing(Timestamp(42, 1), Timestamp(41, 9)));
    ASSERT_FALSE(isMonotonicallyIncreasing(Timestamp(99, 0), Timestamp(10, 10)));
    ASSERT_FALSE(isMonotonicallyIncreasing(Timestamp(123, 5), Timestamp(99, 2)));

    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(0, 0), Timestamp(0, 0)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(0, 0), Timestamp(0, 1)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(0, 123), Timestamp(99, 0)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(0, 0), Timestamp(123, 0)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(0, 0), Timestamp(123, 123)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(41, 9), Timestamp(41, 9)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(42, 1), Timestamp(43, 0)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(42, 1), Timestamp(42, 1)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(31, 1), Timestamp(41, 9)));
    ASSERT_TRUE(isMonotonicallyIncreasing(Timestamp(42, 100), Timestamp(99, 0)));
}

TEST(AsyncResultsMergerTest, CheckHigResumeTokensAreMonotonicallyIncreasing) {
    // Compare resume tokens against high water mark tokens.
    UUID uuid = UUID::gen();

    auto pbrt1 = makeResumeToken(Timestamp(42, 1), uuid, BSON("_id" << 1));
    BSONObj doc1 = BSON("_id" << pbrt1 << "$sortKey" << BSON_ARRAY(pbrt1) << "value" << 1);

    ASSERT_FALSE(isMonotonicallyIncreasing(makeHighWaterMarkToken(Timestamp(43, 0)), doc1));
    ASSERT_FALSE(isMonotonicallyIncreasing(makeHighWaterMarkToken(Timestamp(43, 1)), doc1));
    ASSERT_TRUE(isMonotonicallyIncreasing(makeHighWaterMarkToken(Timestamp(42, 0)), doc1));
    ASSERT_TRUE(isMonotonicallyIncreasing(makeHighWaterMarkToken(Timestamp(42, 1)), doc1));
    ASSERT_TRUE(isMonotonicallyIncreasing(doc1, doc1));

    auto pbrt2 = makeResumeToken(Timestamp(42, 2), uuid, BSON("_id" << 1));
    BSONObj doc2 = BSON("_id" << pbrt2 << "$sortKey" << BSON_ARRAY(pbrt2) << "value" << 1);

    ASSERT_FALSE(isMonotonicallyIncreasing(doc2, doc1));
    ASSERT_TRUE(isMonotonicallyIncreasing(doc2, doc2));
    ASSERT_TRUE(isMonotonicallyIncreasing(doc1, doc2));
}

DEATH_TEST_REGEX(NextHighWaterMarkDeterminingStrategyTest,
                 InvalidHighWatermarkStrategy,
                 "Tripwire assertion.*10359107") {
    // Use the "invalid" high water mark determining strategy. This strategy will always trigger a
    // tassert when invoked.
    auto nextHighWaterMarkDeterminingStrategy = NextHighWaterMarkDeterminingStrategyFactory::
        createInvalidHighWaterMarkDeterminingStrategy();

    ASSERT_EQ("invalid"_sd, nextHighWaterMarkDeterminingStrategy->getName());

    // Throws whenever this strategy is used.
    ASSERT_THROWS_CODE((*nextHighWaterMarkDeterminingStrategy)(BSONObj(), BSONObj()),
                       AssertionException,
                       10359107);
}

DEATH_TEST_REGEX(NextHighWaterMarkDeterminingStrategyTest,
                 ChangeStreamV1InvalidInputDocument,
                 "Tripwire assertion.*10359101") {
    auto nextHighWaterMarkDeterminingStrategy =
        buildNextHighWaterMarkDeterminingStrategy(false /* recognizeControlEvents */);

    ASSERT_EQ("changeStreamV1"_sd, nextHighWaterMarkDeterminingStrategy->getName());

    // '$sortKey' field is always expected. Will fail when passing in a BSONObj without a '$sortKey'
    // field.
    ASSERT_THROWS_CODE((*nextHighWaterMarkDeterminingStrategy)(BSONObj(), BSONObj()),
                       AssertionException,
                       10359101);
}

TEST(NextHighWaterMarkDeterminingStrategyTest,
     HighWatermarkStrategiesSequenceWithIncreasingTokens) {
    // Build a postBatchResumeToken from the components.
    auto buildPBRT = [&](bool useHighWaterMarkTokens, Timestamp ts, int i) -> BSONObj {
        if (useHighWaterMarkTokens) {
            return AsyncResultsMergerTest::makePostBatchResumeToken(ts);
        }
        return makeResumeToken(ts, UUID::gen(), BSON("_id" << i));
    };

    // Test with and without control events.
    for (bool recognizeControlEvents : {true, false}) {
        // Test with high water mark tokens and document-based resume tokens.
        for (bool useHighWaterMarkTokens : {true, false}) {
            auto nextHighWaterMarkDeterminingStrategy =
                buildNextHighWaterMarkDeterminingStrategy(recognizeControlEvents);

            ASSERT_EQ(recognizeControlEvents ? "recognizeControlEvents"_sd : "changeStreamV1"_sd,
                      nextHighWaterMarkDeterminingStrategy->getName());

            // Send in initial document with resume token. This should return the same high water
            // mark as in the document.
            BSONObj pbrt = buildPBRT(useHighWaterMarkTokens, Timestamp(42, 1), 1);
            BSONObj doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt) << "value" << 1);
            auto next = (*nextHighWaterMarkDeterminingStrategy)(doc, pbrt);
            ASSERT_BSONOBJ_EQ(pbrt, next.firstElement().Obj());

            // Send a document with a higher resume token. This should return an updated high water
            // mark with the same value as in the document just sent.
            pbrt = buildPBRT(useHighWaterMarkTokens, Timestamp(42, 2), 2);
            doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt) << "value" << 2);

            // Use same input document again. This should return the same high water mark as before.
            next = (*nextHighWaterMarkDeterminingStrategy)(doc, next);
            ASSERT_BSONOBJ_EQ(pbrt, next.firstElement().Obj());

            // Send a document with an even higher resume token. This should return an updated high
            // water
            // mark with the same value as in the document just sent.
            pbrt = buildPBRT(useHighWaterMarkTokens, Timestamp(43, 0), 3);
            doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt) << "value" << 3);

            next = (*nextHighWaterMarkDeterminingStrategy)(doc, next);
            ASSERT_BSONOBJ_EQ(pbrt, next.firstElement().Obj());
        }
    }
}

DEATH_TEST_REGEX(NextHighWaterMarkDeterminingStrategyTest,
                 RecognizeControlEventsInvalidInputDocument,
                 "Tripwire assertion.*10359101") {
    auto nextHighWaterMarkDeterminingStrategy =
        buildNextHighWaterMarkDeterminingStrategy(true /* recognizeControlEvents */);

    ASSERT_EQ("recognizeControlEvents"_sd, nextHighWaterMarkDeterminingStrategy->getName());

    // '$sortKey' field is always expected in the input document. Will fail when passing in a
    // BSONObj without a '$sortKey' field.
    BSONObj pbrt = makeResumeToken(Timestamp(23, 1), UUID::gen(), BSON("_id" << 1));
    ASSERT_THROWS_CODE(
        (*nextHighWaterMarkDeterminingStrategy)(BSONObj(), pbrt), AssertionException, 10359101);
}

DEATH_TEST_REGEX(NextHighWaterMarkDeterminingStrategyTest,
                 RecognizeControlEventsThrowsUponEmptyCurrentHighWaterMark,
                 "Tripwire assertion.*10359109") {
    auto nextHighWaterMarkDeterminingStrategy =
        buildNextHighWaterMarkDeterminingStrategy(true /* recognizeControlEvents */);

    BSONObj pbrt = makeResumeToken(Timestamp(23, 1), UUID::gen(), BSON("_id" << 1));
    BSONObj doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt)
                             << Document::metaFieldChangeStreamControlEvent << 1 << "value"
                             << "control1");
    ASSERT_THROWS_CODE(
        (*nextHighWaterMarkDeterminingStrategy)(doc, BSONObj()), AssertionException, 10359109);
}

TEST(NextHighWaterMarkDeterminingStrategyTest,
     RecognizeControlEventsControlEventWithHigherTimestampThanPrevious) {
    auto nextHighWaterMarkDeterminingStrategy =
        buildNextHighWaterMarkDeterminingStrategy(true /* recognizeControlEvents */);

    // Set initial high water mark from document and expect that the same value is returned.
    BSONObj pbrt = makeResumeToken(Timestamp(23, 1), UUID::gen(), BSON("_id" << 1));
    BSONObj doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt) << "value" << 1);
    BSONObj next = (*nextHighWaterMarkDeterminingStrategy)(doc, pbrt);
    ASSERT_BSONOBJ_EQ(pbrt, next.firstElement().Obj());

    // Build control event with higher timestamp than initial document. This should return an
    // updated high water mark token.
    pbrt = makeResumeToken(Timestamp(42, 1), UUID::gen(), BSON("_id" << 1));
    doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt)
                     << Document::metaFieldChangeStreamControlEvent << 1 << "value"
                     << "control1");
    next = (*nextHighWaterMarkDeterminingStrategy)(doc, next);
    ASSERT_BSONOBJ_EQ(AsyncResultsMergerTest::makePostBatchResumeToken(Timestamp(42, 1)),
                      next.firstElement().Obj());
}

TEST(NextHighWaterMarkDeterminingStrategyTest,
     RecognizeControlEventsControlEventWithSameTimestampAsPrevious) {
    auto nextHighWaterMarkDeterminingStrategy =
        buildNextHighWaterMarkDeterminingStrategy(true /* recognizeControlEvents */);

    // Set initial high water mark from document and expect that the same value is returned.
    BSONObj pbrt = makeResumeToken(Timestamp(23, 1), UUID::gen(), BSON("_id" << 1));
    BSONObj doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt) << "value" << 1);
    BSONObj next = (*nextHighWaterMarkDeterminingStrategy)(doc, pbrt);
    ASSERT_BSONOBJ_EQ(pbrt, next.firstElement().Obj());

    // Build control event with same timestamp as initial document. This should return the same high
    // water mark.
    doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt)
                     << Document::metaFieldChangeStreamControlEvent << 1 << "value"
                     << "control1");
    next = (*nextHighWaterMarkDeterminingStrategy)(doc, next);
    ASSERT_BSONOBJ_EQ(pbrt, next.firstElement().Obj());
}

}  // namespace
}  // namespace mongo
