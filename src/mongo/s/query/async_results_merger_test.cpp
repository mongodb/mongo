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

using ResponseStatus = executor::TaskExecutor::ResponseStatus;

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

    void tearDown() override {
        ShardingTestFixture::tearDown();
        // Reset _params only after shutting down the network interface (through
        // ShardingTestFixture::tearDown()), because shutting down the network interface will
        // deliver blackholed responses to the AsyncResultsMerger, and the AsyncResultsMerger's
        // callback may still access _params.
        _params.reset();
    }

protected:
    /**
     * Constructs an ARM with the given vector of existing cursors.
     *
     * If 'findCmd' is not set, the default ClusterClientCursorParams are used.
     * Otherwise, the 'findCmd' is used to construct the ClusterClientCursorParams.
     *
     * 'findCmd' should not have a 'batchSize', since the find's batchSize is used just in the
     * initial find. The getMore 'batchSize' can be passed in through 'getMoreBatchSize.'
     */
    void makeCursorFromExistingCursors(
        std::vector<ClusterClientCursorParams::RemoteCursor> remoteCursors,
        boost::optional<BSONObj> findCmd = boost::none,
        boost::optional<long long> getMoreBatchSize = boost::none,
        ReadPreferenceSetting readPref = ReadPreferenceSetting(ReadPreference::PrimaryOnly)) {
        _params = stdx::make_unique<ClusterClientCursorParams>(_nss, UserNameIterator(), readPref);
        _params->remotes = std::move(remoteCursors);

        if (findCmd) {
            const auto qr = unittest::assertGet(
                QueryRequest::makeFromFindCommand(_nss, *findCmd, false /* isExplain */));
            _params->sort = qr->getSort();
            _params->limit = qr->getLimit();
            _params->batchSize = getMoreBatchSize ? getMoreBatchSize : qr->getBatchSize();
            _params->skip = qr->getSkip();
            _params->isTailable = qr->isTailable();
            _params->isAwaitData = qr->isAwaitData();
            _params->isAllowPartialResults = qr->isAllowPartialResults();
        }

        arm = stdx::make_unique<AsyncResultsMerger>(executor(), _params.get());
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

    const NamespaceString _nss;
    std::unique_ptr<ClusterClientCursorParams> _params;

    std::unique_ptr<AsyncResultsMerger> arm;
};

TEST_F(AsyncResultsMergerTest, SingleShardUnsorted) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{$sortKey: {'': 5}}"), fromjson("{$sortKey: {'': 6}}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 6, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {
        fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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

    // After returning all the buffered results, the ARM returns EOF immediately because both
    // shards cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, MultiShardSorted) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {_id: 1}}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 6, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{$sortKey: {'': 5}}"),
                                   fromjson("{$sortKey: {'': 6}}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    // ARM is not ready to return results until receiving responses from all remotes.
    ASSERT_FALSE(arm->ready());

    // ARM is not exhausted, because second shard has yet to respond.
    ASSERT_FALSE(arm->remotesExhausted());

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{$sortKey: {'': 3}}"),
                                   fromjson("{$sortKey: {'': 9}}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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

    // After returning all the buffered results, the ARM returns EOF immediately because both
    // shards cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, MultiShardMultipleGets) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 6, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    // Before any requests are scheduled, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Schedule requests.
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // First shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(5), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Second shard responds; the handleBatchResponse callback is run and ARM's remote gets updated.
    responses.clear();
    std::vector<BSONObj> batch2 = {
        fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // First shard returns remainder of results.
    responses.clear();
    std::vector<BSONObj> batch3 = {
        fromjson("{_id: 7}"), fromjson("{_id: 8}"), fromjson("{_id: 9}")};
    responses.emplace_back(_nss, CursorId(0), batch3);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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

    // After returning all the buffered results, the ARM returns EOF immediately because both
    // shards cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, CompoundSortKey) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {a: -1, b: 1}}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 6, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 7, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    // Schedule requests.
    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // Deliver responses.
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
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
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

    // After returning all the buffered results, the ARM returns EOF immediately because both
    // shards cursors were exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, SortedButNoSortKey) {
    BSONObj findCmd = fromjson("{find: 'testcoll', sort: {a: -1, b: 1}}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // Parsing the batch results in an error because the sort key is missing.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{a: 2, b: 1}"), fromjson("{a: 1, b: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::InternalError);

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, HasFirstBatch) {
    std::vector<BSONObj> firstBatch = {
        fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")};
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, std::move(firstBatch)));
    makeCursorFromExistingCursors(std::move(cursors));

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
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 5, std::move(firstBatch)));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 0, {}));
    makeCursorFromExistingCursors(std::move(cursors));

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
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Before any responses are delivered, ARM is not ready to return results.
    ASSERT_FALSE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());

    // Shard responds; the handleBatchResponse callbacks are run and ARM's remotes get updated.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 2, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // Both shards respond with the first batch.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(2), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
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
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
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
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 5}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 6}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // We can continue to return results from first shard, while second shard remains unresponsive.
    responses.clear();
    std::vector<BSONObj> batch4 = {fromjson("{_id: 7}"), fromjson("{_id: 8}")};
    responses.emplace_back(_nss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 7}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 8}"), *unittest::assertGet(arm->nextReady()).getResult());

    // Kill cursor before deleting it, as the second remote cursor has not been exhausted. We don't
    // wait on 'killEvent' here, as the blackholed request's callback will only run on shutdown of
    // the network interface.
    auto killEvent = arm->kill(nullptr);
    ASSERT_TRUE(killEvent.isValid());
}

TEST_F(AsyncResultsMergerTest, ErrorOnMismatchedCursorIds) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 4}"), fromjson("{_id: 5}"), fromjson("{_id: 6}")};
    responses.emplace_back(_nss, CursorId(456), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT(!arm->nextReady().isOK());

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, BadResponseReceivedFromShard) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 456, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 789, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    BSONObj response1 = CursorResponse(_nss, CursorId(123), batch1)
                            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    BSONObj response2 = fromjson("{foo: 'bar'}");
    std::vector<BSONObj> batch3 = {fromjson("{_id: 4}"), fromjson("{_id: 5}")};
    BSONObj response3 = CursorResponse(_nss, CursorId(789), batch3)
                            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    scheduleNetworkResponseObjs({response1, response2, response3});
    executor()->waitForEvent(readyEvent);
    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ErrorReceivedFromShard) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 2, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 3, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(2), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    scheduleErrorResponse({ErrorCodes::BadValue, "bad thing happened"});
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    auto statusWithNext = arm->nextReady();
    ASSERT(!statusWithNext.isOK());
    ASSERT_EQ(statusWithNext.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(statusWithNext.getStatus().reason(), "bad thing happened");

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ErrorCantScheduleEventBeforeLastSignaled) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    // Error to call nextEvent() before the previous event is signaled.
    ASSERT_NOT_OK(arm->nextEvent(nullptr).getStatus());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, NextEventAfterTaskExecutorShutdown) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    executor()->shutdown();
    ASSERT_EQ(ErrorCodes::ShutdownInProgress, arm->nextEvent(nullptr).getStatus());
    auto killEvent = arm->kill(nullptr);
    ASSERT_FALSE(killEvent.isValid());
}

TEST_F(AsyncResultsMergerTest, KillAfterTaskExecutorShutdownWithOutstandingBatches) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    // Make a request to the shard that will never get answered.
    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());
    blackHoleNextRequest();

    // Executor shuts down before a response is received.
    executor()->shutdown();
    auto killEvent = arm->kill(nullptr);
    ASSERT_FALSE(killEvent.isValid());
}

TEST_F(AsyncResultsMergerTest, KillNoBatchesRequested) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto killedEvent = arm->kill(nullptr);

    // Killed cursors are considered ready, but return an error when you try to receive the next
    // doc.
    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());

    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillAllBatchesReceived) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 2, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    std::vector<BSONObj> batch3 = {fromjson("{_id: 3}"), fromjson("{_id: 4}")};
    responses.emplace_back(_nss, CursorId(123), batch3);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    // Kill should be able to return right away if there are no pending batches.
    auto killedEvent = arm->kill(nullptr);
    ASSERT_TRUE(arm->ready());
    ASSERT_NOT_OK(arm->nextReady().getStatus());
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillTwoOutstandingBatches) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 2, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 3, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    // Kill event will only be signalled once the callbacks for the pending batches have run.
    auto killedEvent = arm->kill(nullptr);

    // The pending requests have been canceled, so run their callbacks.
    runReadyCallbacks();

    // Ensure that we properly signal those waiting for more results to be ready.
    executor()->waitForEvent(readyEvent);
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, NextEventErrorsAfterKill) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

    auto killedEvent = arm->kill(nullptr);

    // Attempting to schedule more network operations on a killed arm is an error.
    ASSERT_NOT_OK(arm->nextEvent(nullptr).getStatus());

    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, KillCalledTwice) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors));
    auto killedEvent1 = arm->kill(nullptr);
    ASSERT(killedEvent1.isValid());
    auto killedEvent2 = arm->kill(nullptr);
    ASSERT(killedEvent2.isValid());
    executor()->waitForEvent(killedEvent1);
    executor()->waitForEvent(killedEvent2);
}

TEST_F(AsyncResultsMergerTest, TailableBasic) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
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
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(123), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_FALSE(arm->remotesExhausted());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_FALSE(arm->remotesExhausted());

    auto killedEvent = arm->kill(nullptr);
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, TailableEmptyBatch) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // Remote responds with an empty batch and a non-zero cursor id.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch;
    responses.emplace_back(_nss, CursorId(123), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    // After receiving an empty batch, the ARM should return boost::none, but remotes should not be
    // marked as exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_FALSE(arm->remotesExhausted());

    auto killedEvent = arm->kill(nullptr);
    executor()->waitForEvent(killedEvent);
}

TEST_F(AsyncResultsMergerTest, TailableExhaustedCursor) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // Remote responds with an empty batch and a zero cursor id.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch;
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    // Afterwards, the ARM should return boost::none and remote cursors should be marked as
    // exhausted.
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
    ASSERT_TRUE(arm->remotesExhausted());
}

TEST_F(AsyncResultsMergerTest, GetMoreBatchSizes) {
    BSONObj findCmd = fromjson("{find: 'testcoll', batchSize: 3}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(1), batch1);

    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_FALSE(arm->ready());

    responses.clear();

    std::vector<BSONObj> batch2 = {fromjson("{_id: 3}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));

    BSONObj scheduledCmd = getFirstPendingRequest().cmdObj;
    auto request = GetMoreRequest::parseFromBSON("anydbname", scheduledCmd);
    ASSERT_OK(request.getStatus());
    ASSERT_EQ(*request.getValue().batchSize, 1LL);
    ASSERT_EQ(request.getValue().cursorid, 1LL);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, SendsSecondaryOkAsMetadata) {
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    makeCursorFromExistingCursors(std::move(cursors),
                                  boost::none,
                                  boost::none,
                                  ReadPreferenceSetting(ReadPreference::Nearest));

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    BSONObj cmdRequestMetadata = getFirstPendingRequest().metadata;
    ASSERT_BSONOBJ_EQ(cmdRequestMetadata, rpc::ServerSelectionMetadata(true, boost::none).toBSON());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(0), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, AllowPartialResults) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 97, {}));
    cursors.emplace_back(kTestShardHosts[1], CursorResponse(_nss, 98, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 99, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // An error occurs with the first host.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
    ASSERT_FALSE(arm->ready());

    // Instead of propagating the error, we should be willing to return results from the two
    // remaining shards.
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(98), batch1);
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(99), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
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
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 3}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // Once the last reachable shard indicates that its cursor is closed, we're done.
    responses.clear();
    std::vector<BSONObj> batch4 = {};
    responses.emplace_back(_nss, CursorId(0), batch4);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, AllowPartialResultsSingleNode) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 98, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(98), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // The lone host involved in this query returns an error. This should simply cause us to return
    // EOF.
    scheduleErrorResponse({ErrorCodes::AuthenticationFailed, "authentication failed"});
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}

TEST_F(AsyncResultsMergerTest, AllowPartialResultsOnRetriableErrorNoRetries) {
    BSONObj findCmd = fromjson("{find: 'testcoll', allowPartialResults: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 2, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // First host returns single result
    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(0), batch);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);

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
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 1, {}));
    cursors.emplace_back(kTestShardHosts[2], CursorResponse(_nss, 2, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
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
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestIncludesMaxTimeMS) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true, awaitData: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    std::vector<CursorResponse> responses;
    std::vector<BSONObj> batch1 = {fromjson("{_id: 1}")};
    responses.emplace_back(_nss, CursorId(123), batch1);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());

    ASSERT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));

    ASSERT_FALSE(arm->ready());
    readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    ASSERT_FALSE(arm->ready());

    // Pending getMore request should include maxTimeMS.
    BSONObj expectedCmdObj = BSON("getMore" << CursorId(123) << "collection"
                                            << "testcoll"
                                            << "maxTimeMS"
                                            << 789);
    ASSERT_BSONOBJ_EQ(getFirstPendingRequest().cmdObj, expectedCmdObj);

    responses.clear();
    std::vector<BSONObj> batch2 = {fromjson("{_id: 2}")};
    responses.emplace_back(_nss, CursorId(0), batch2);
    scheduleNetworkResponses(std::move(responses),
                             CursorResponse::ResponseType::SubsequentResponse);
    executor()->waitForEvent(readyEvent);

    ASSERT_TRUE(arm->ready());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), *unittest::assertGet(arm->nextReady()).getResult());
    ASSERT_TRUE(arm->ready());
    ASSERT_TRUE(unittest::assertGet(arm->nextReady()).isEOF());
}


TEST_F(AsyncResultsMergerTest, GetMoreRequestWithoutTailableCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll'}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, GetMoreRequestWithoutAwaitDataCantHaveMaxTime) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_NOT_OK(arm->setAwaitDataTimeout(Milliseconds(789)));
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

TEST_F(AsyncResultsMergerTest, ShardCanErrorInBetweenReadyAndNextEvent) {
    BSONObj findCmd = fromjson("{find: 'testcoll', tailable: true}");
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardHosts[0], CursorResponse(_nss, 123, {}));
    makeCursorFromExistingCursors(std::move(cursors), findCmd);

    ASSERT_FALSE(arm->ready());
    auto readyEvent = unittest::assertGet(arm->nextEvent(nullptr));
    scheduleErrorResponse({ErrorCodes::BadValue, "bad thing happened"});

    ASSERT_EQ(ErrorCodes::BadValue, arm->nextEvent(nullptr).getStatus());

    // Required to kill the 'arm' on error before destruction.
    auto killEvent = arm->kill(nullptr);
    executor()->waitForEvent(killEvent);
}

}  // namespace

}  // namespace mongo
