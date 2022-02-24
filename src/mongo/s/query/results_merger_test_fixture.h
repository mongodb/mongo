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

#pragma once

#include "mongo/s/query/async_results_merger.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

/**
 * Test fixture which is useful to both the tests for AsyncResultsMerger and BlockingResultsMerger.
 */
class ResultsMergerTestFixture : public ShardingTestFixture {
public:
    static const HostAndPort kTestConfigShardHost;
    static const std::vector<ShardId> kTestShardIds;
    static const std::vector<HostAndPort> kTestShardHosts;

    static const NamespaceString kTestNss;

    ResultsMergerTestFixture() {}

    void setUp() override;

protected:
    /**
     * Constructs an AsyncResultsMergerParams object with the given vector of existing cursors.
     *
     * If 'findCmd' is not set, the default AsyncResultsMergerParams are used. Otherwise, the
     * 'findCmd' is used to construct the AsyncResultsMergerParams.
     *
     * 'findCmd' should not have a 'batchSize', since the find's batchSize is used just in the
     * initial find. The getMore 'batchSize' can be passed in through 'getMoreBatchSize.'
     */
    AsyncResultsMergerParams makeARMParamsFromExistingCursors(
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

        OperationSessionInfoFromClient sessionInfo;
        boost::optional<LogicalSessionFromClient> lsidFromClient;

        if (auto lsid = operationContext()->getLogicalSessionId()) {
            lsidFromClient.emplace(lsid->getId());
            lsidFromClient->setUid(lsid->getUid());
        }

        sessionInfo.setSessionId(lsidFromClient);
        sessionInfo.setTxnNumber(operationContext()->getTxnNumber());
        params.setOperationSessionInfo(sessionInfo);
        return params;
    }
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

        return stdx::make_unique<AsyncResultsMerger>(
            operationContext(),
            executor(),
            makeARMParamsFromExistingCursors(std::move(remoteCursors), findCmd, getMoreBatchSize));
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
            // For tests of the AsyncResultsMerger, all CursorResponses scheduled by the tests are
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
            executor::RemoteCommandResponse response(obj, millis);
            executor::TaskExecutor::ResponseStatus responseStatus(response);
            net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
        }
        net->runReadyNetworkOperations();
        net->exitNetwork();
    }

    executor::RemoteCommandRequest getNthPendingRequest(size_t n) {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        executor::NetworkInterfaceMock::NetworkOperationIterator noi =
            net->getNthUnscheduledRequest(n);
        executor::RemoteCommandRequest retRequest = noi->getRequest();
        net->exitNetwork();
        return retRequest;
    }

    bool networkHasReadyRequests() {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        return guard->hasReadyRequests();
    }

    void scheduleErrorResponse(executor::TaskExecutor::ResponseStatus rs) {
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

    ClockSourceMock* getMockClockSource() {
        ClockSourceMock* mockClock = dynamic_cast<ClockSourceMock*>(
            operationContext()->getServiceContext()->getPreciseClockSource());
        invariant(mockClock);
        return mockClock;
    }
};

}  // namespace mongo
