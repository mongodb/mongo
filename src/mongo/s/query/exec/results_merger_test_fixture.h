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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/query/exec/async_results_merger.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/next_high_watermark_determining_strategy.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Test fixture which is useful to both the tests for AsyncResultsMerger and BlockingResultsMerger.
 */
class ResultsMergerTestFixture : public virtual service_context_test::RouterRoleOverride,
                                 public ShardingTestFixture {
public:
    static const HostAndPort kTestConfigShardHost;
    static const std::vector<ShardId> kTestShardIds;
    static const std::vector<HostAndPort> kTestShardHosts;

    static const NamespaceString kTestNss;

    ResultsMergerTestFixture() = default;

    void setUp() override;

    static BSONObj makePostBatchResumeToken(Timestamp clusterTime) {
        auto pbrt =
            ResumeToken::makeHighWaterMarkToken(clusterTime, ResumeTokenData::kDefaultTokenVersion)
                .toDocument()
                .toBson();
        invariant(pbrt.firstElement().type() == BSONType::String);
        return pbrt;
    }

    static AsyncResultsMergerParams buildARMParamsForChangeStream() {
        const NamespaceString kTestNss =
            NamespaceString::createNamespaceString_forTest("testdb.testcoll");

        AsyncResultsMergerParams params;

        params.setNss(kTestNss);
        params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
        params.setCompareWholeSortKey(false);
        params.setSort(change_stream_constants::kSortSpec);

        return params;
    }

protected:
    std::shared_ptr<AsyncResultsMerger> buildARM(AsyncResultsMergerParams params,
                                                 bool recognizeControlEvents = false) {
        NextHighWaterMarkDeterminingStrategyPtr nextHighWaterMarkDeterminingStrategy;
        if (params.getTailableMode().value_or(TailableModeEnum::kNormal) ==
            TailableModeEnum::kTailableAndAwaitData) {
            nextHighWaterMarkDeterminingStrategy =
                NextHighWaterMarkDeterminingStrategyFactory::createForChangeStream(
                    params, recognizeControlEvents);
        }

        auto arm = AsyncResultsMerger::create(operationContext(), executor(), std::move(params));
        if (nextHighWaterMarkDeterminingStrategy) {
            arm->setNextHighWaterMarkDeterminingStrategy(
                std::move(nextHighWaterMarkDeterminingStrategy));
        }
        return arm;
    }

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
            // If there is no '$db', append it.
            auto cmd =
                OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(operationContext()),
                                            kTestNss.dbName(),
                                            *findCmd)
                    .body;
            const auto findCommand =
                query_request_helper::makeFromFindCommandForTests(cmd, kTestNss);
            if (!findCommand->getSort().isEmpty()) {
                params.setSort(findCommand->getSort().getOwned());
            }

            if (getMoreBatchSize) {
                params.setBatchSize(getMoreBatchSize);
            } else {
                params.setBatchSize(findCommand->getBatchSize()
                                        ? boost::optional<std::int64_t>(static_cast<std::int64_t>(
                                              *findCommand->getBatchSize()))
                                        : boost::none);
            }
            params.setTailableMode(query_request_helper::getTailableMode(*findCommand));
            params.setAllowPartialResults(findCommand->getAllowPartialResults());
        }

        if (auto lsid = operationContext()->getLogicalSessionId()) {
            OperationSessionInfoFromClient sessionInfo([&] {
                LogicalSessionFromClient lsidFromClient(lsid->getId());
                lsidFromClient.setUid(lsid->getUid());
                return lsidFromClient;
            }());
            sessionInfo.setTxnNumber(operationContext()->getTxnNumber());
            params.setOperationSessionInfo(sessionInfo);
        }
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
    std::shared_ptr<AsyncResultsMerger> makeARMFromExistingCursors(
        std::vector<RemoteCursor> remoteCursors,
        boost::optional<BSONObj> findCmd = boost::none,
        boost::optional<std::int64_t> getMoreBatchSize = boost::none) {

        return buildARM(
            makeARMParamsFromExistingCursors(std::move(remoteCursors), findCmd, getMoreBatchSize),
            false /* recognizeControlEvents */);
    }

    /**
     * Runs tests for the ARM's high water mark, which is set initially and then updated from
     * multiple responses.
     */
    void runHighWaterMarkTest(bool recognizeControlEvents) {
        // Create an AsyncResultsMerger without any remote cursors.
        auto arm = buildARM(buildARMParamsForChangeStream(), recognizeControlEvents);
        ASSERT_EQ(0, arm->getNumRemotes());

        // No high water mark set initially.
        ASSERT_BSONOBJ_EQ(BSONObj(), arm->getHighWaterMark());

        // Inject a high water mark.
        auto highWaterMark = makePostBatchResumeToken(Timestamp(42, 1));

        arm->setInitialHighWaterMark(highWaterMark);

        ASSERT_BSONOBJ_EQ(highWaterMark, arm->getHighWaterMark());

        // Add a remote cursor.
        auto pbrt = makePostBatchResumeToken(Timestamp(42, 2));
        std::vector<RemoteCursor> cursors;
        cursors.push_back(makeRemoteCursor(kTestShardIds[0],
                                           kTestShardHosts[0],
                                           CursorResponse(kTestNss, 1, {}, boost::none, pbrt)));
        arm->addNewShardCursors(std::move(cursors));

        // Expect PBRT from the first batch, and not the original high water mark.
        ASSERT_BSONOBJ_EQ(pbrt, arm->getHighWaterMark());

        auto readyEvent = unittest::assertGet(arm->nextEvent());

        // Deliver response with an updated PBRT.
        BSONObj oldPBRT = pbrt;
        std::vector<CursorResponse> responses;
        pbrt = makePostBatchResumeToken(Timestamp(42, 3));
        BSONObj doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt) << "value" << 1);
        std::vector<BSONObj> batch1 = {doc};
        responses.emplace_back(kTestNss, CursorId(1), batch1, boost::none, pbrt);
        scheduleNetworkResponses(std::move(responses));

        executor()->waitForEvent(readyEvent);

        ASSERT_TRUE(arm->ready());

        // Still expect the same high water mark as before.
        ASSERT_BSONOBJ_EQ(oldPBRT, arm->getHighWaterMark());

        // Consume next buffered document. This updates the high watermark.
        ASSERT_BSONOBJ_EQ(doc, *unittest::assertGet(arm->nextReady()).getResult());
        ASSERT_BSONOBJ_EQ(pbrt, arm->getHighWaterMark());
        ASSERT_FALSE(arm->ready());

        readyEvent = unittest::assertGet(arm->nextEvent());

        // Deliver empty response with the same PBRT.
        responses.clear();
        std::vector<BSONObj> batch2 = {};
        responses.emplace_back(kTestNss, CursorId(1), batch2, boost::none, pbrt);
        scheduleNetworkResponses(std::move(responses));

        executor()->waitForEvent(readyEvent);

        // No change expected for the high water mark.
        ASSERT_FALSE(arm->ready());
        ASSERT_BSONOBJ_EQ(pbrt, arm->getHighWaterMark());

        readyEvent = unittest::assertGet(arm->nextEvent());

        // Deliver response with an updated PBRT.
        oldPBRT = pbrt;
        responses.clear();
        pbrt = makePostBatchResumeToken(Timestamp(42, 4));
        doc = BSON("_id" << pbrt << "$sortKey" << BSON_ARRAY(pbrt) << "value" << 2);
        std::vector<BSONObj> batch3 = {doc};
        responses.emplace_back(kTestNss, CursorId(1), batch3, boost::none, pbrt);

        scheduleNetworkResponses(std::move(responses));

        executor()->waitForEvent(readyEvent);

        // High water mark should not have been updated by receiving the batch.
        ASSERT_TRUE(arm->ready());
        ASSERT_BSONOBJ_EQ(oldPBRT, arm->getHighWaterMark());

        // Consume next buffered document. This should update the high watermark.
        ASSERT_BSONOBJ_EQ(doc, *unittest::assertGet(arm->nextReady()).getResult());
        ASSERT_BSONOBJ_EQ(pbrt, arm->getHighWaterMark());
        ASSERT_FALSE(arm->ready());

        // Close the cursor. Now the ARM manages no open cursors.
        arm->closeShardCursors({kTestShardIds[0]});
        ASSERT_EQ(0, arm->getNumRemotes());

        // Still expect the same high water mark as before.
        ASSERT_BSONOBJ_EQ(pbrt, arm->getHighWaterMark());
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
            executor::RemoteCommandResponse response =
                executor::RemoteCommandResponse::make_forTest(obj, millis);
            executor::TaskExecutor::ResponseStatus responseStatus(response);
            net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
        }
        net->runReadyNetworkOperations();
        net->exitNetwork();
    }

    size_t getNumPendingRequests() {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        size_t numPending = net->getNumReadyRequests();
        net->exitNetwork();
        return numPending;
    }

    executor::RemoteCommandRequest getNthPendingRequest(size_t n) {
        executor::NetworkInterfaceMock* net = network();
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        executor::NetworkInterfaceMock::NetworkOperationIterator noi = net->getNthReadyRequest(n);
        // Ensure that we have a valid iterator before dereferencing it.
        ASSERT_FALSE(net->isNetworkOperationIteratorAtEnd(noi));
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
        rs.elapsed = Milliseconds(0);
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

    void assertKillCursorsCmdHasCursorId(const BSONObj& killCmd, CursorId cursorId) {
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
