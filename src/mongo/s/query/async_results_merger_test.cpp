
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

#include "mongo/s/query/async_results_merger.h"

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using ResponseStatus = executor::TaskExecutor::ResponseStatus;

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

const NamespaceString kTestNss("testdb.testcoll");

LogicalSessionId parseSessionIdFromCmd(BSONObj cmdObj) {
    return LogicalSessionId::parse(IDLParserErrorContext("lsid"), cmdObj["lsid"].Obj());
}

class AsyncResultsMergerTest : public ShardingTestFixture {
public:
    AsyncResultsMergerTest() {}

    void setUp() override {
        setRemote(HostAndPort("ClientHost", 12345));

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;

        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());

            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                stdx::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

protected:
    /**
     * Constructs an ARM with the given vector of existing cursors.
     *
     * If 'findCmd' is not set, the default AsyncResultsMergerParams are used.
     * Otherwise, the 'findCmd' is used to construct the AsyncResultsMergerParams.
     *
     * 'findCmd' should not have a 'batchSize', since the find's batchSize is used just in the
     * initial find. The getMore 'batchSize' can be passed in through 'getMoreBatchSize.'
     */
    std::unique_ptr<AsyncResultsMerger> makeARMFromExistingCursors(
        std::vector<RemoteCursor> remoteCursors,
        boost::optional<BSONObj> findCmd = boost::none,
        boost::optional<std::int64_t> getMoreBatchSize = boost::none) {
        AsyncResultsMergerParams params;
        params.setNss(kTestNss);
        params.setRemotes(std::move(remoteCursors));


        if (findCmd) {
            const auto qr = unittest::assertGet(
                QueryRequest::makeFromFindCommand(kTestNss, *findCmd, false /* isExplain */));
            if (!qr->getSort().isEmpty()) {
                params.setSort(qr->getSort().getOwned());
            }

            if (getMoreBatchSize) {
                params.setBatchSize(getMoreBatchSize);
            } else {
                params.setBatchSize(qr->getBatchSize()
                                        ? boost::optional<std::int64_t>(
                                              static_cast<std::int64_t>(*qr->getBatchSize()))
                                        : boost::none);
            }
            params.setTailableMode(qr->getTailableMode());
            params.setAllowPartialResults(qr->isAllowPartialResults());
        }

        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(operationContext()->getLogicalSessionId());
        sessionInfo.setTxnNumber(operationContext()->getTxnNumber());
        params.setOperationSessionInfo(sessionInfo);

        return stdx::make_unique<AsyncResultsMerger>(
            operationContext(), executor(), std::move(params));
    }

    /**
     * Schedules a "CommandOnShardedViewNotSupportedOnMongod" error response w/ view definition.
     */
    void scheduleNetworkViewResponse(const std::string& ns, const std::string& pipelineJsonArr) {
        BSONObjBuilder viewDefBob;
        viewDefBob.append("ns", ns);
        viewDefBob.append("pipeline", fromjson(pipelineJsonArr));

        BSONObjBuilder bob;
        bob.append("resolvedView", viewDefBob.obj());
        bob.append("ok", 0.0);
        bob.append("errmsg", "Command on view must be executed by mongos");
        bob.append("code", 169);

        std::vector<BSONObj> batch = {bob.obj()};
        scheduleNetworkResponseObjs(batch);
    }

    /**
     * Schedules a list of cursor responses to be returned by the mock network.
     */
    void scheduleNetworkResponses(std::vector<CursorResponse> responses) {
        std::vector<BSONObj> objs;
        for (const auto& cursorResponse : responses) {
            // For tests of the AsyncResultsMerger, all CursorRepsonses scheduled by the tests are
            // subsequent responses, since the AsyncResultsMerger will only ever run getMores.
            objs.push_back(cursorResponse.toBSON(CursorResponse::ResponseType::SubsequentResponse));
        }
        scheduleNetworkResponseObjs(objs);
    }

    /**
     * Schedules a single cursor response to be returned by the mock network.
     */
    void scheduleNetworkResponse(CursorResponse&& response) {
        std::vector<CursorResponse> responses;
        responses.push_back(std::move(response));
        scheduleNetworkResponses(std::move(responses));
    }

    /**
     * Schedules a list of raw BSON command responses to be returned by the mock network.
     */
    void scheduleNetworkResponseObjs(std::vector<BSONObj> objs) {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        for (const auto& obj : objs) {
            ASSERT_TRUE(net->hasReadyRequests());
            Milliseconds millis(0);
            RemoteCommandResponse response(obj, BSONObj(), millis);
            executor::TaskExecutor::ResponseStatus responseStatus(response);
            net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
        }
        net->runReadyNetworkOperations();
        net->exitNetwork();
    }

    RemoteCommandRequest getNthPendingRequest(size_t n) {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        NetworkInterfaceMock::NetworkOperationIterator noi = net->getNthUnscheduledRequest(n);
        RemoteCommandRequest retRequest = noi->getRequest();
        net->exitNetwork();
        return retRequest;
    }

    bool networkHasReadyRequests() {
        NetworkInterfaceMock::InNetworkGuard guard(network());
        return guard->hasReadyRequests();
    }

    void scheduleErrorResponse(ResponseStatus rs) {
        invariant(!rs.isOK());
        rs.elapsedMillis = Milliseconds(0);
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), rs);
        net->runReadyNetworkOperations();
        net->exitNetwork();
    }

    void runReadyCallbacks() {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        net->runReadyNetworkOperations();
        net->exitNetwork();
    }

    void blackHoleNextRequest() {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        net->blackHole(net->getNextReadyRequest());
        net->exitNetwork();
    }
};

void assertKillCusorsCmdHasCursorId(const BSONObj& killCmd, CursorId cursorId) {
    ASSERT_TRUE(killCmd.hasElement("killCursors"));
    ASSERT_EQ(killCmd["cursors"].type(), BSONType::Array);

    size_t numCursors = 0;
    for (auto&& cursor : killCmd["cursors"].Obj()) {
        ASSERT_EQ(cursor.type(), BSONType::NumberLong);
        ASSERT_EQ(cursor.numberLong(), cursorId);
        ++numCursors;
    }
    ASSERT_EQ(numCursors, 1u);
}

RemoteCursor makeRemoteCursor(ShardId shardId, HostAndPort host, CursorResponse response) {
    RemoteCursor remoteCursor;
    remoteCursor.setShardId(std::move(shardId));
    remoteCursor.setHostAndPort(std::move(host));
    remoteCursor.setCursorResponse(std::move(response));
    return remoteCursor;
}

static const auto kTokenFormat = ResumeToken::SerializationFormat::kHexString;

BSONObj makePostBatchResumeToken(Timestamp clusterTime) {
    auto pbrt =
        ResumeToken::makeHighWaterMarkResumeToken(clusterTime).toDocument(kTokenFormat).toBson();
    invariant(pbrt.firstElement().type() == BSONType::String);
    return pbrt;
}

BSONObj makeResumeToken(Timestamp clusterTime, UUID uuid, BSONObj docKey) {
    ResumeTokenData data;
    data.clusterTime = clusterTime;
    data.uuid = uuid;
    data.documentKey = Value(Document{docKey});
    return ResumeToken(data).toDocument(kTokenFormat).toBson();
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
    std::vector<BSONObj> batch = {fromjson("{$sortKey: {'': 5}}"), fromjson("{$sortKey: {'': 6}}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // Now that the responses have been delivered, ARM is ready to return results.
    ASSERT_TRUE(arm->ready());

    // Because the response contained a cursorId of 0, ARM marked the remote as exhausted.
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns all results in order.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 5}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 6}}"),
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
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: {'': 5}}"),
                                   fromjson("{$sortKey: {'': 6}}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses));

    // ARM is not ready to return results until receiving responses from all remotes.
    ASSERT_FALSE(arm->ready());

    // ARM is not exhausted, because second shard has yet to respond.
    ASSERT_FALSE(arm->remotesExhausted());

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: {'': 3}}"),
                                   fromjson("{$sortKey: {'': 9}}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses));

    // Now that all remotes have responded, ARM is ready to return results.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());

    // ARM returns all results in sorted order.
    executor()->waitForEvent(readyEvent);
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 3}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 5}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 6}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 9}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());

    // After returning all the buffered results, the ARM returns EOF immediately because both shards
    // cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
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
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: {'': 5, '': 9}}"),
                                   fromjson("{$sortKey: {'': 4, '': 20}}")};
    responses.emplace_back(kTestNss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: {'': 10, '': 11}}"),
                                   fromjson("{$sortKey: {'': 4, '': 4}}")};
    responses.emplace_back(kTestNss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{$sortKey: {'': 10, '': 12}}"),
                                   fromjson("{$sortKey: {'': 5, '': 9}}")};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

    // ARM returns all results in sorted order.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 10, '': 11}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 10, '': 12}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 5, '': 9}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 5, '': 9}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 4, '': 4}}"),
                      *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{$sortKey: {'': 4, '': 20}}"),
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
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
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

    // Kill cursor before deleting it, as the second remote cursor has not been exhausted. We don't
    // wait on 'killEvent' here, as the blackholed request's callback will only run on shutdown of
    // the network interface.
    auto killEvent = arm->kill(operationContext());
    ASSERT_TRUE(killEvent.isValid());
    executor()->shutdown();
    executor()->waitForEvent(killEvent);
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
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
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
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
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

    scheduleErrorResponse({ErrorCodes::BadValue, "bad thing happened"});
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(statusWithNext.getStatus().reason(), "bad thing happened");

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
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
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, NextEventAfterTaskExecutorShutdown) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    executor()->shutdown();
    ASSERT_EQ(ErrorCodes::ShutdownInProgress, arm->nextEvent().getStatus());
    auto killEvent = arm->kill(operationContext());
    ASSERT_FALSE(killEvent.isValid());
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
    executor()->shutdown();
    auto killEvent = arm->kill(operationContext());
    ASSERT_FALSE(killEvent.isValid());

    // Ensure that the executor finishes all of the outstanding callbacks before the ARM is freed.
    executor()->join();
}

TEST_F(AsyncResultsMergerTest, KillNoBatchesRequested) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto killedEvent = arm->kill(operationContext());
    assertKillCusorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    // Killed cursors are considered ready, but return an error when you try to receive the next
    // doc.
    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());

    executor()->waitForEvent(killedEvent);
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

    auto killedEvent = arm->kill(operationContext());

    // ARM shouldn't schedule killCursors on anything since all of the remotes are exhausted.
    ASSERT_FALSE(networkHasReadyRequests());

    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());
    executor()->waitForEvent(killedEvent);
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

    auto killedEvent = arm->kill(operationContext());

    // ARM should schedule killCursors on cursor 123
    assertKillCusorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 123);

    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());
    executor()->waitForEvent(killedEvent);
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
    auto killedEvent = arm->kill(operationContext());

    // Check that the ARM kills both batches.
    assertKillCusorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 2);
    assertKillCusorsCmdHasCursorId(getNthPendingRequest(1u).cmdObj, 3);

    // Run the callbacks which were canceled.
    runReadyCallbacks();

    // Ensure that we properly signal those waiting for more results to be ready.
    executor()->waitForEvent(readyEvent);
    executor()->waitForEvent(killedEvent);
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

    auto killedEvent = arm->kill(operationContext());

    // Attempting to schedule more network operations on a killed arm is an error.
    ASSERT_NOT_OK(arm->nextEvent().getStatus());

    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillCalledTwice) {
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));
    auto killedEvent1 = arm->kill(operationContext());
    ASSERT(killedEvent1.isValid());
    auto killedEvent2 = arm->kill(operationContext());
    ASSERT(killedEvent2.isValid());
    executor()->waitForEvent(killedEvent1);
    executor()->waitForEvent(killedEvent2);
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

    auto killedEvent = arm->kill(operationContext());
    executor()->waitForEvent(killedEvent);
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

    auto killedEvent = arm->kill(operationContext());
    executor()->waitForEvent(killedEvent);
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
    auto request = GetMoreRequest::parseFromBSON("anydbname", scheduledCmd);
    ASSERT_OK(request.getStatus());
    ASSERT_EQ(*request.getValue().batchSize, 1LL);
    ASSERT_EQ(request.getValue().cursorid, 1LL);
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

    // An error occurs with the first host.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
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

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Now the second host becomes unreachable. We should still be willing to return results from
    // the third shard.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(99), batch3);
    scheduleNetworkResponses(std::move(responses));
    executor()->waitForEvent(readyEvent);

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

    // The lone host involved in this query returns an error. This should simply cause us to return
    // EOF.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
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

    // First host returns single result
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(kTestNss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses));

    // From the second host we get a network (retriable) error.
    scheduleErrorResponse({ErrorCodes::HostUnreachable, "host unreachable"});

    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());

    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());

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

    // Both hosts return network (retriable) errors.
    scheduleErrorResponse({ErrorCodes::HostUnreachable, "host unreachable"});
    scheduleErrorResponse({ErrorCodes::HostUnreachable, "host unreachable"});

    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());

    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::HostUnreachable);
    ASSERT_EQ(statusWithNext.getStatus().reason(), "host unreachable");

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestIncludesMaxTimeMS) {
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
                                    << "maxTimeMS"
                                    << 789);
    ASSERT_BSONOBJ_EQ(getNthPendingRequest(0).cmdObj, expectedCmdObj);

    // Clean up.
    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}")};
    responses.emplace_back(kTestNss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses));
}

DEATH_TEST_F(AsyncResultsMergerTest,
             SortedTailableInvariantsIfInitialBatchHasNoPostBatchResumeToken,
             "Invariant failure _promisedMinSortKeys.empty() || _promisedMinSortKeys.size() == "
             "_remotes.size()") {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    // Create one cursor whose initial response has a postBatchResumeToken.
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: {'': '"
                      << firstDocSortKey.firstElement().String()
                      << "'}}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(
            kTestNss, 123, {firstCursorResponse}, boost::none, boost::none, pbrtFirstCursor)));
    // Create a second cursor whose initial batch has no PBRT.
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(kTestNss, 456, {})));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);
    auto arm =
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params));

    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // We should be dead by now.
    MONGO_UNREACHABLE;
}

DEATH_TEST_F(AsyncResultsMergerTest,
             SortedTailableCursorInvariantsIfOneOrMoreRemotesHasEmptyPostBatchResumeToken,
             "Invariant failure !response.getPostBatchResumeToken()->isEmpty()") {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    BSONObj pbrtFirstCursor;
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: {'': '"
                      << firstDocSortKey.firstElement().String()
                      << "'}}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(
            kTestNss, 123, {firstCursorResponse}, boost::none, boost::none, pbrtFirstCursor)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);
    auto arm =
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params));

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
                      << "', documentKey: {_id: 1}}, $sortKey: {'': '"
                      << firstDocSortKey.firstElement().String()
                      << "'}}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(
            kTestNss, 123, {firstCursorResponse}, boost::none, boost::none, pbrtFirstCursor)));
    auto tooLow = makePostBatchResumeToken(Timestamp(1, 2));
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(kTestNss, 456, {}, boost::none, boost::none, tooLow)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);
    auto arm =
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params));

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    ASSERT_FALSE(arm->ready());

    // Clean up the cursors.
    std::vector<CursorResponse> responses;
    responses.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{});
    scheduleNetworkResponses(std::move(responses));
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest,
       SortedTailableCursorAssertsIfRemoteHasOplogTimestampButNoPostBatchResumeToken) {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: {'': '"
                      << firstDocSortKey.firstElement().String()
                      << "'}}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(
            kTestNss, 123, {firstCursorResponse}, boost::none, Timestamp(1, 5), boost::none)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);
    ASSERT_THROWS_CODE(
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params)),
        AssertionException,
        ErrorCodes::InternalErrorNotSupported);
}

// Test that merging is done purely on the basis of the postBatchResumeToken.
TEST_F(AsyncResultsMergerTest, SortedTailableCursorIgnoresOplogTimestamp) {
    AsyncResultsMergerParams params;
    params.setNss(kTestNss);
    UUID uuid = UUID::gen();
    std::vector<RemoteCursor> cursors;
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    // Set the first cursor to have both a PBRT and a matching oplog timestamp.
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: {'': '"
                      << firstDocSortKey.firstElement().String()
                      << "'}}");
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(
            kTestNss, 123, {firstCursorResponse}, boost::none, Timestamp(1, 5), pbrtFirstCursor)));

    // Set the second cursor to have a PBRT that comes after the first cursor's, but an oplog time
    // that preceds it. If merging used the oplog timestamp, then the arm would not be ready.
    auto pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 6));
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[1],
        kTestShardHosts[1],
        CursorResponse(kTestNss, 456, {}, boost::none, Timestamp(1, 1), pbrtSecondCursor)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);
    auto arm =
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params));

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // The ARM is ready, indicating that it used the PBRT rather than the oplog timestamp.
    ASSERT_TRUE(arm->ready());

    // Clean up the cursors.
    std::vector<CursorResponse> responses;
    responses.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{});
    scheduleNetworkResponses(std::move(responses));
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
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
    auto arm =
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params));

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    ASSERT_FALSE(arm->ready());

    // Schedule one response with an oplog timestamp in it.
    std::vector<CursorResponse> responses;
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 6));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: {'': '"
                      << firstDocSortKey.firstElement().String()
                      << "'}}");
    std::vector<BSONObj> batch1 = {firstCursorResponse};
    auto firstDoc = batch1.front();
    responses.emplace_back(
        kTestNss, CursorId(123), batch1, boost::none, boost::none, pbrtFirstCursor);
    scheduleNetworkResponses(std::move(responses));

    // Should be ready now.
    ASSERT_TRUE(arm->ready());

    // Add the new shard.
    auto tooLowPBRT = makePostBatchResumeToken(Timestamp(1, 3));
    std::vector<RemoteCursor> newCursors;
    newCursors.push_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(kTestNss, 456, {}, boost::none, boost::none, tooLowPBRT)));
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
                      << "', documentKey: {_id: 2}}, $sortKey: {'': '"
                      << secondDocSortKey.firstElement().String()
                      << "'}}");
    std::vector<BSONObj> batch2 = {secondCursorResponse};
    auto secondDoc = batch2.front();
    responses.emplace_back(
        kTestNss, CursorId(456), batch2, boost::none, boost::none, pbrtSecondCursor);
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
    auto arm =
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params));

    auto readyEvent = unittest::assertGet(arm->nextEvent());

    ASSERT_FALSE(arm->ready());

    // Schedule one response with an oplog timestamp in it.
    std::vector<CursorResponse> responses;
    auto firstDocSortKey = makeResumeToken(Timestamp(1, 4), uuid, BSON("_id" << 1));
    auto pbrtFirstCursor = makePostBatchResumeToken(Timestamp(1, 5));
    auto firstCursorResponse = fromjson(
        str::stream() << "{_id: {clusterTime: {ts: Timestamp(1, 4)}, uuid: '" << uuid.toString()
                      << "', documentKey: {_id: 1}}, $sortKey: {'': '"
                      << firstDocSortKey.firstElement().String()
                      << "'}}");
    std::vector<BSONObj> batch1 = {firstCursorResponse};
    responses.emplace_back(
        kTestNss, CursorId(123), batch1, boost::none, boost::none, pbrtFirstCursor);
    scheduleNetworkResponses(std::move(responses));

    // Should be ready now.
    ASSERT_TRUE(arm->ready());

    // Add the new shard.
    auto tooLowPBRT = makePostBatchResumeToken(Timestamp(1, 3));
    std::vector<RemoteCursor> newCursors;
    newCursors.push_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(kTestNss, 456, {}, boost::none, boost::none, tooLowPBRT)));
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
                      << "', documentKey: {_id: 2}}, $sortKey: {'': '"
                      << secondDocSortKey.firstElement().String()
                      << "'}}");
    std::vector<BSONObj> batch2 = {secondCursorResponse};
    // The last observed time should still be later than the first shard, so we can get the data
    // from it.
    responses.emplace_back(
        kTestNss, CursorId(456), batch2, boost::none, boost::none, pbrtSecondCursor);
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
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[0],
        kTestShardHosts[0],
        CursorResponse(kTestNss, 123, {}, boost::none, boost::none, pbrtFirstCursor)));
    auto pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 1));
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[1],
        kTestShardHosts[1],
        CursorResponse(kTestNss, 456, {}, boost::none, boost::none, pbrtSecondCursor)));
    auto pbrtThirdCursor = makePostBatchResumeToken(Timestamp(1, 4));
    cursors.push_back(makeRemoteCursor(
        kTestShardIds[2],
        kTestShardHosts[2],
        CursorResponse(kTestNss, 789, {}, boost::none, boost::none, pbrtThirdCursor)));
    params.setRemotes(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    params.setSort(change_stream_constants::kSortSpec);
    auto arm =
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params));

    // We have no results to return, so the ARM is not ready.
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // The high water mark should be the second cursor's PBRT, since it is the lowest of the three.
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtSecondCursor);

    // Advance the PBRT of the second cursor. It should still be the lowest. The fixture expects
    // each cursor to be updated in-order, so we keep the first and third PBRTs constant.
    pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 3));
    std::vector<BSONObj> emptyBatch = {};
    scheduleNetworkResponse(
        {kTestNss, CursorId(123), emptyBatch, boost::none, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse(
        {kTestNss, CursorId(456), emptyBatch, boost::none, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse(
        {kTestNss, CursorId(789), emptyBatch, boost::none, boost::none, pbrtThirdCursor});
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtSecondCursor);
    ASSERT_FALSE(arm->ready());

    // Advance the second cursor again, so that it surpasses the other two. The third cursor becomes
    // the new high water mark.
    pbrtSecondCursor = makePostBatchResumeToken(Timestamp(1, 6));
    scheduleNetworkResponse(
        {kTestNss, CursorId(123), emptyBatch, boost::none, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse(
        {kTestNss, CursorId(456), emptyBatch, boost::none, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse(
        {kTestNss, CursorId(789), emptyBatch, boost::none, boost::none, pbrtThirdCursor});
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtThirdCursor);
    ASSERT_FALSE(arm->ready());

    // Advance the third cursor such that the first cursor becomes the high water mark.
    pbrtThirdCursor = makePostBatchResumeToken(Timestamp(1, 7));
    scheduleNetworkResponse(
        {kTestNss, CursorId(123), emptyBatch, boost::none, boost::none, pbrtFirstCursor});
    scheduleNetworkResponse(
        {kTestNss, CursorId(456), emptyBatch, boost::none, boost::none, pbrtSecondCursor});
    scheduleNetworkResponse(
        {kTestNss, CursorId(789), emptyBatch, boost::none, boost::none, pbrtThirdCursor});
    ASSERT_BSONOBJ_EQ(arm->getHighWaterMark(), pbrtFirstCursor);
    ASSERT_FALSE(arm->ready());

    // Clean up the cursors.
    std::vector<BSONObj> cleanupBatch = {};
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
    scheduleNetworkResponse({kTestNss, CursorId(0), cleanupBatch});
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestWithoutTailableCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestWithoutAwaitDataCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ShardCanErrorInBetweenReadyAndNextEvent) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<RemoteCursor> cursors;
    cursors.push_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 123, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    scheduleErrorResponse({ErrorCodes::BadValue, "bad thing happened"});

    ASSERT_EQ(ErrorCodes::BadValue, arm->nextEvent().getStatus());

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(operationContext());
    executor()->waitForEvent(killEvent);
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
    auto killEvent = arm->kill(operationContext());

    // Check that the ARM will run killCursors.
    assertKillCusorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    // Let the callback run now that it's been canceled.
    runReadyCallbacks();

    executor()->waitForEvent(readyEvent);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ShouldBeAbleToBlockUntilNextResultIsReady) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto next = unittest::assertGet(arm->blockingNext());
        ASSERT_FALSE(next.isEOF());
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));
        next = unittest::assertGet(arm->blockingNext());
        ASSERT_TRUE(next.isEOF());
    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(AsyncResultsMergerTest, ShouldBeInterruptableDuringBlockingNext) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto nextStatus = arm->blockingNext();
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::Interrupted);
    });

    // Now mark the OperationContext as killed from this thread.
    {
        stdx::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->markKilled(ErrorCodes::Interrupted);
    }
    future.timed_get(kFutureTimeout);
    // Be careful not to use a blocking kill here, since the main thread is in charge of running the
    // callbacks, and we'd block on ourselves.
    auto killEvent = arm->kill(operationContext());

    assertKillCusorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);
    runReadyCallbacks();
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ShouldBeAbleToBlockUntilKilled) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto arm = makeARMFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    arm->blockingKill(operationContext());
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

        return CursorResponse(kTestNss, 1LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
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

        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });
}

DEATH_TEST_F(AsyncResultsMergerTest,
             ConstructingARMWithTxnNumAndNoLSIDShouldCrash,
             "Invariant failure params.getSessionId()") {
    AsyncResultsMergerParams params;

    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(5);
    params.setOperationSessionInfo(sessionInfo);

    // This should trigger an invariant.
    ASSERT_FALSE(
        stdx::make_unique<AsyncResultsMerger>(operationContext(), executor(), std::move(params)));
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

    auto killedEvent = arm->kill(operationContext());
    executor()->waitForEvent(killedEvent);
}

}  // namespace
}  // namespace mongo
