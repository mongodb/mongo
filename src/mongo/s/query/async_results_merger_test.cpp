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

#include "mongo/s/query/async_results_merger.h"

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/json.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

class AsyncResultsMergerTest : public ShardingTestFixture {
public:
    AsyncResultsMergerTest() : _nss("testdb.testcoll") {}

    void setUp() override {
        ShardingTestFixture::setUp();
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
     * Given a find command specification, 'findCmd', and a list of remote host:port pairs,
     * constructs the appropriate ARM.
     *
     * If 'batchSize' is set (i.e. not equal to boost::none), this batchSize is used for each
     * getMore. If 'findCmd' has a batchSize, this is used just for the initial find operation.
     */
    void makeCursorFromFindCmd(
        const BSONObj& findCmd,
        const std::vector<ShardId>& shardIds,
        boost::optional<long long> getMoreBatchSize = boost::none,
        ReadPreferenceSetting readPref = ReadPreferenceSetting(ReadPreference::PrimaryOnly)) {
        const bool isExplain = true;
        const auto qr =
            unittest::assertGet(QueryRequest::makeFromFindCommand(_nss, findCmd, isExplain));

        ClusterClientCursorParams params = ClusterClientCursorParams(_nss, readPref);
        params.sort = qr->getSort();
        params.limit = qr->getLimit();
        params.batchSize = getMoreBatchSize ? getMoreBatchSize : qr->getBatchSize();
        params.skip = qr->getSkip();
        params.isTailable = qr->isTailable();
        params.isAwaitData = qr->isAwaitData();
        params.isAllowPartialResults = qr->isAllowPartialResults();

        for (const auto& shardId : shardIds) {
            params.remotes.emplace_back(shardId, findCmd);
        }

        arm = stdx::make_unique<AsyncResultsMerger>(executor(), std::move(params));
    }

    /**
     * Given a vector of (HostAndPort, CursorIds) representing a set of existing cursors, constructs
     * the appropriate ARM.  The default CCC parameters are used.
     */
    void makeCursorFromExistingCursors(
        const std::vector<std::pair<HostAndPort, CursorId>>& remotes) {
        ClusterClientCursorParams params = ClusterClientCursorParams(_nss);

        for (const auto& hostIdPair : remotes) {
            params.remotes.emplace_back(hostIdPair.first, hostIdPair.second);
        }

        arm = stdx::make_unique<AsyncResultsMerger>(executor(), std::move(params));
    }

    /**
     * Schedules a list of cursor responses to be returned by the mock network.
     */
    void scheduleNetworkResponses(std::vector<CursorResponse> responses,
                                  CursorResponse::ResponseType responseType) {
        std::vector<BSONObj> objs;
        for (const auto& cursorResponse : responses) {
            objs.push_back(cursorResponse.toBSON(responseType));
        }
        scheduleNetworkResponseObjs(objs);
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

    RemoteCommandRequest getFirstPendingRequest() {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        NetworkInterfaceMock::NetworkOperationIterator noi = net->getFrontOfUnscheduledQueue();
        RemoteCommandRequest retRequest = noi->getRequest();
        net->exitNetwork();
        return retRequest;
    }

    void scheduleErrorResponse(Status status) {
        invariant(!status.isOK());
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), status);
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

    const NamespaceString _nss;

    std::unique_ptr<AsyncResultsMerger> arm;
};

TEST_F(AsyncResultsMergerTest, ClusterFind) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // First shard responds.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    // Can't return any results until we have a response from all three shards.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Second two shards respond.
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, ClusterFindAndGetMore) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(10), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(11), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(12), batch3);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_FALSE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    responses.clear();
    std::vector<BSONObj> batch4 = {fromjson("{_id: 7}"), fromjson("{_id: 8}")};
    responses.emplace_back(_nss, CursorId(10), batch4);
    std::vector<BSONObj> batch5 = {fromjson("{_id: 9}")};
    responses.emplace_back(_nss, CursorId(0), batch5);
    std::vector<BSONObj> batch6 = {fromjson("{_id: 10}")};
    responses.emplace_back(_nss, CursorId(0), batch6);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_FALSE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 10}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 7}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 8}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 9}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    responses.clear();
    std::vector<BSONObj> batch7 = {fromjson("{_id: 11}")};
    responses.emplace_back(_nss, CursorId(0), batch7);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 11}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, ClusterFindSorted) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}, batchSize: 2}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 5, $sortKey: {'': 5}}"),
                                   fromjson("{_id: 6, $sortKey: {'': 6}}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3, $sortKey: {'': 3}}"),
                                   fromjson("{_id: 9, $sortKey: {'': 9}}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{_id: 4, $sortKey: {'': 4}}"),
                                   fromjson("{_id: 8, $sortKey: {'': 8}}")};
    responses.emplace_back(_nss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3, $sortKey: {'': 3}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 4, $sortKey: {'': 4}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 5, $sortKey: {'': 5}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 6, $sortKey: {'': 6}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 8, $sortKey: {'': 8}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 9, $sortKey: {'': 9}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, ClusterFindAndGetMoreSorted) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}, batchSize: 2}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: {'': 5}}"),
                                   fromjson("{$sortKey: {'': 6}}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: {'': 3}}"),
                                   fromjson("{$sortKey: {'': 4}}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{$sortKey: {'': 7}}"),
                                   fromjson("{$sortKey: {'': 8}}")};
    responses.emplace_back(_nss, CursorId(2), batch3);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 3}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 4}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 5}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 6}}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch4 = {fromjson("{$sortKey: {'': 7}}"),
                                   fromjson("{$sortKey: {'': 10}}")};
    responses.emplace_back(_nss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 7}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 7}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 8}}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch5 = {fromjson("{$sortKey: {'': 9}}"),
                                   fromjson("{$sortKey: {'': 10}}")};
    responses.emplace_back(_nss, CursorId(0), batch5);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 9}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 10}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 10}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, ClusterFindCompoundSortKey) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {a: -1, b: 1}, batchSize: 2}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: {'': 5, '': 9}}"),
                                   fromjson("{$sortKey: {'': 4, '': 20}}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: {'': 10, '': 11}}"),
                                   fromjson("{$sortKey: {'': 4, '': 4}}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{$sortKey: {'': 10, '': 12}}"),
                                   fromjson("{$sortKey: {'': 5, '': 9}}")};
    responses.emplace_back(_nss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 10, '': 11}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 10, '': 12}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 5, '': 9}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 5, '': 9}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 4, '': 4}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{$sortKey: {'': 4, '': 20}}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, ClusterFindSortedButNoSortKey) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {a: -1, b: 1}, batchSize: 2}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Parsing the batch results in an error because the sort key is missing.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{a: 2, b: 1}"), fromjson("{a: 1, b: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::InternalError);

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}


TEST_F(AsyncResultsMergerTest, ClusterFindInitialBatchSizeIsZero) {
    // Initial batchSize sent with the find command is zero; batchSize sent with each getMore
    // command is one.
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 0}");
    const long long getMoreBatchSize = 1LL;
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0], kTestShardIds[1]}, getMoreBatchSize);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Both shards give back empty responses. Second shard doesn't have any results so it
    // sends back a cursor id of zero.
    std::vector<CursorResponse> responses;
    responses.emplace_back(_nss, CursorId(1), std::vector<BSONObj>());
    responses.emplace_back(_nss, CursorId(0), std::vector<BSONObj>());
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    // In handling the responses from the first shard, the ARM should have already asked
    // for an additional batch from that shard. It won't have anything to return until it
    // gets a non-empty response.
    ASSERT_FALSE(arm->ready());
    responses.clear();
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // The shard responds with another empty batch but leaves the cursor open. It probably shouldn't
    // do this, but there's no reason the ARM can't handle this by asking for more.
    responses.clear();
    responses.emplace_back(_nss, CursorId(1), std::vector<BSONObj>());
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    // The shard responds with another batch and closes the cursor.
    ASSERT_FALSE(arm->ready());
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, ExistingCursors) {
    makeCursorFromExistingCursors({{kTestShardHosts[0], 5}, {kTestShardHosts[1], 6}});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}


TEST_F(AsyncResultsMergerTest, StreamResultsFromOneShardIfOtherDoesntRespond) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0], kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Both shards respond with the first batch.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(2), batch2);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 4}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // When we ask the shards for their next batch, the first shard responds and the second shard
    // never responds.
    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(1), batch3);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    blackHoleNextRequest();
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // We can continue to return results from first shard, while second shard remains unresponsive.
    responses.clear();
    std::vector<BSONObj> batch4 = {fromjson("{_id: 7}"), fromjson("{_id: 8}")};
    responses.emplace_back(_nss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 7}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 8}"), *unittest::assertGet(arm->nextReady()));

    // Kill cursor before deleting it, as the second remote cursor has not been exhausted. We don't
    // wait on 'killEvent' here, as the blackholed request's callback will only run on shutdown of
    // the network interface.
    auto killEvent = arm->kill();
    ASSERT_TRUE(killEvent.isValid());
}

TEST_F(AsyncResultsMergerTest, ErrorOnMismatchedCursorIds) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_FALSE(arm->ready());

    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch2 = {
        fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(456), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT(!arm->nextReady().isOK());

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, BadResponseReceivedFromShard) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    BSONObj response1 = CursorResponse(_nss, CursorId(123), batch1)
                            .toBSON(CursorResponse::ResponseType::InitialResponse);
    BSONObj response2 = fromjson("{foo: 'bar'}");
    std::vector<BSONObj> batch3 = {fromjson("{_id: 4}"), fromjson("{_id: 5}")};
    BSONObj response3 = CursorResponse(_nss, CursorId(456), batch3)
                            .toBSON(CursorResponse::ResponseType::InitialResponse);
    scheduleNetworkResponseObjs({response1, response2, response3});
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ErrorReceivedFromShard) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(2), batch2);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    scheduleErrorResponse({ErrorCodes::BadValue, "bad thing happened"});
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(statusWithNext.getStatus().reason(), "bad thing happened");

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ErrorCantScheduleEventBeforeLastSignaled) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());

    // Error to call nextEvent() before the previous event is signaled.
    ASSERT_NOT_OK(arm->nextEvent().getStatus());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, NextEventAfterTaskExecutorShutdown) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);
    executor()->shutdown();
    ASSERT_NOT_OK(arm->nextEvent().getStatus());
    auto killEvent = arm->kill();
    ASSERT_FALSE(killEvent.isValid());
}

TEST_F(AsyncResultsMergerTest, KillAfterTaskExecutorShutdownWithOutstandingBatches) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    // Make a request to the shard that will never get answered.
    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());
    blackHoleNextRequest();

    // Executor shuts down before a response is received.
    executor()->shutdown();
    auto killEvent = arm->kill();
    ASSERT_FALSE(killEvent.isValid());
}

TEST_F(AsyncResultsMergerTest, KillNoBatchesRequested) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto killedEvent = arm->kill();

    // Killed cursors are considered ready, but return an error when you try to receive the next
    // doc.
    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());

    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillAllBatchesReceived) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(123), batch3);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    // Kill should be able to return right away if there are no pending batches.
    auto killedEvent = arm->kill();
    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillTwoOutstandingBatches) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    // Kill event will only be signalled once the pending batches are received.
    auto killedEvent = arm->kill();

    // After the kill, the ARM waits for outstanding batches to come back. This ensures that we
    // receive cursor ids for any established remote cursors, and can clean them up by issuing
    // killCursors commands.
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(123), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{_id: 4}"), fromjson("{_id: 5}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    // Only one of the responses has a non-zero cursor id. The ARM should have issued a killCursors
    // command against this id.
    BSONObj expectedCmdObj = BSON("killCursors"
                                  << "testcoll"
                                  << "cursors"
                                  << BSON_ARRAY(CursorId(123)));
    ASSERT_EQ(getFirstPendingRequest().cmdObj, expectedCmdObj);

    // Ensure that we properly signal both those waiting for the kill, and those waiting for more
    // results to be ready.
    executor()->waitForEvent(readyEvent);
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillOutstandingGetMore) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    // First batch received.
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));

    // This will schedule a getMore on cursor id 123.
    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    auto killedEvent = arm->kill();

    // The kill can't complete until the getMore response is received.
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(123), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    // While processing the getMore response, a killCursors against id 123 should have been
    // scheduled.
    BSONObj expectedCmdObj = BSON("killCursors"
                                  << "testcoll"
                                  << "cursors"
                                  << BSON_ARRAY(CursorId(123)));
    ASSERT_EQ(getFirstPendingRequest().cmdObj, expectedCmdObj);

    // Ensure that we properly signal both those waiting for the kill, and those waiting for more
    // results to be ready.
    executor()->waitForEvent(readyEvent);
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, NextEventErrorsAfterKill) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    auto killedEvent = arm->kill();

    // Attempting to schedule more network operations on a killed arm is an error.
    ASSERT_NOT_OK(arm->nextEvent().getStatus());

    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillCalledTwice) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});
    auto killedEvent1 = arm->kill();
    ASSERT(killedEvent1.isValid());
    auto killedEvent2 = arm->kill();
    ASSERT(killedEvent2.isValid());
    executor()->waitForEvent(killedEvent1);
    executor()->waitForEvent(killedEvent2);
}

TEST_F(AsyncResultsMergerTest, TailableBasic) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));

    // In the tailable case, we expect boost::none after every batch.
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
    ASSERT_FALSE(arm->remotesExhausted());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(123), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
    ASSERT_FALSE(arm->remotesExhausted());

    auto killedEvent = arm->kill();
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, TailableEmptyBatch) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Remote responds with an empty batch and a non-zero cursor id.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch;
    responses.emplace_back(_nss, CursorId(123), batch);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    // After receiving an empty batch, the ARM should return boost::none, but remotes should not be
    // marked as exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
    ASSERT_FALSE(arm->remotesExhausted());

    auto killedEvent = arm->kill();
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, TailableExhaustedCursor) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Remote responds with an empty batch and a zero cursor id.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch;
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    // Afterwards, the ARM should return boost::none and remote cursors should be marked as
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST_F(AsyncResultsMergerTest, GetMoreBatchSizes) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 3}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);

    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_FALSE(arm->ready());

    responses.clear();

    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    readyEvent = unittest::assertGet(arm->nextEvent());

    BSONObj scheduledCmd = getFirstPendingRequest().cmdObj;
    auto request = GetMoreRequest::parseFromBSON("anydbname", scheduledCmd);
    ASSERT_OK(request.getStatus());
    ASSERT_EQ(*request.getValue().batchSize, 1LL);
    ASSERT_EQ(request.getValue().cursorid, 1LL);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, SendsSecondaryOkAsMetadata) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 2}");
    makeCursorFromFindCmd(
        findCmd, {kTestShardIds[0]}, boost::none, ReadPreferenceSetting(ReadPreference::Nearest));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    BSONObj cmdRequestMetadata = getFirstPendingRequest().metadata;
    ASSERT_EQ(cmdRequestMetadata, rpc::ServerSelectionMetadata(true, boost::none).toBSON());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, AllowPartialResults) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    makeCursorFromFindCmd(findCmd, kTestShardIds);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // An unretriable error occurs with the first host.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
    ASSERT_FALSE(arm->ready());

    // Instead of propagating the error, we should be willing to return results from the two
    // remaining shards.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(98), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(99), batch2);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Now the second host becomes unreachable. We should still be willing to return results from
    // the third shard.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(99), batch3);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Once the last reachable shard indicates that its cursor is closed, we're done.
    responses.clear();
    std::vector<BSONObj> batch4 = {};
    responses.emplace_back(_nss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, AllowPartialResultsSingleNode) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(98), batch);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // The lone host involved in this query returns an error. This should simply cause us to return
    // EOF.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, RetryOnNotMasterNoSlaveOkSingleNode) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // First and second attempts return an error.
    scheduleErrorResponse({ErrorCodes::NotMasterNoSlaveOk, "not master and not slave"});
    ASSERT_FALSE(arm->ready());

    scheduleErrorResponse({ErrorCodes::NotMasterNoSlaveOk, "not master and not slave"});
    ASSERT_FALSE(arm->ready());

    // Third attempt succeeds.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
}

TEST_F(AsyncResultsMergerTest, RetryOnNotMasterNoSlaveOkAllFailSingleNode) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // All attempts return an error (one attempt plus three retries)
    scheduleErrorResponse({ErrorCodes::NotMasterNoSlaveOk, "not master and not slave"});
    ASSERT_FALSE(arm->ready());

    scheduleErrorResponse({ErrorCodes::NotMasterNoSlaveOk, "not master and not slave"});
    ASSERT_FALSE(arm->ready());

    scheduleErrorResponse({ErrorCodes::NotMasterNoSlaveOk, "not master and not slave"});
    ASSERT_FALSE(arm->ready());

    scheduleErrorResponse({ErrorCodes::NotMasterNoSlaveOk, "not master and not slave"});
    ASSERT_TRUE(arm->ready());

    auto status = arm->nextReady();
    ASSERT_EQ(status.getStatus().code(), ErrorCodes::NotMasterNoSlaveOk);

    // Protocol is to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, RetryOnHostUnreachableAllowPartialResults) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0], kTestShardIds[1]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // First host returns single result
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);

    // From the second host all attempts return an error (one attempt plus three retries)
    scheduleErrorResponse({ErrorCodes::HostUnreachable, "host unreachable"});
    ASSERT_FALSE(arm->ready());

    scheduleErrorResponse({ErrorCodes::HostUnreachable, "host unreachable"});
    ASSERT_FALSE(arm->ready());

    scheduleErrorResponse({ErrorCodes::HostUnreachable, "host unreachable"});
    ASSERT_FALSE(arm->ready());

    scheduleErrorResponse({ErrorCodes::HostUnreachable, "host unreachable"});
    ASSERT_TRUE(arm->ready());

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));

    ASSERT_TRUE(arm->remotesExhausted());
    ASSERT_TRUE(arm->ready());
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestIncludesMaxTimeMS) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true, awaitData: true}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses), CursorResponse::ResponseType::InitialResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));

    ASSERT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent());
    ASSERT_FALSE(arm->ready());

    // Pending getMore request should include maxTimeMS.
    BSONObj expectedCmdObj = BSON("getMore" << CursorId(123) << "collection"
                                            << "testcoll"
                                            << "maxTimeMS"
                                            << 789);
    ASSERT_EQ(getFirstPendingRequest().cmdObj, expectedCmdObj);

    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()));
    ASSERT_TRUE(arm->ready());
    ASSERT(!unittest::assertGet(arm->nextReady()));
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestWithoutTailableCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});
    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestWithoutAwaitDataCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    makeCursorFromFindCmd(findCmd, {kTestShardIds[0]});
    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killEvent = arm->kill();
    executor()->waitForEvent(killEvent);
}

}  // namespace

}  // namespace mongo
