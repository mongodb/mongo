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


#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/factory.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ClusterAggregateTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kAggregateCmdTargeted{
        fromjson("{aggregate: 'coll', pipeline: [{$match: {_id: 0}}], explain: false, "
                 "allowDiskUse: false, cursor: {batchSize: 10}, maxTimeMS: 100}")};

    const BSONObj kAggregateCmdScatterGather{
        fromjson("{aggregate: 'coll', pipeline: [], explain: false, allowDiskUse: false, cursor: "
                 "{batchSize: 10}}")};

    // The index of the shard expected to receive the response is used to prevent different shards
    // from returning documents with the same shard key. This is expected to be 0 for queries
    // targeting one shard.
    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);

            BSONObjBuilder bob;
            const auto cursorResponseObj =
                cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
            bob.appendElementsUnique(cursorResponseObj);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            cb(request);

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);

            BSONObjBuilder bob;
            const auto cursorResponseObj =
                cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
            bob.appendElementsUnique(cursorResponseObj);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    /**
     * This method should only be used to test early exits from Cluster::runAggregate, before
     * a request is sent to the shards. Otherwise the call would get blocked as no expect* hooks
     * are provided in this method.
     */
    Status testRunAggregateEarlyExit(const BSONObj& inputBson) {
        BSONObjBuilder result;
        auto client = getServiceContext()->getService()->makeClient("ClusterCmdClient");
        auto opCtx = client->makeOperationContext();
        auto request = aggregation_request_helper::parseFromBSONForTests(inputBson);
        if (request.getStatus() != Status::OK()) {
            return request.getStatus();
        }

        auto nss = request.getValue().getNamespace();
        return ClusterAggregate::runAggregate(opCtx.get(),
                                              ClusterAggregate::Namespaces{nss, nss},
                                              request.getValue(),
                                              {request.getValue()},
                                              PrivilegeVector(),
                                              boost::none, /* verbosity */
                                              &result);
    }
};

TEST_F(ClusterAggregateTest, NoErrors) {
    testNoErrors(kAggregateCmdTargeted, kAggregateCmdScatterGather);
}

TEST_F(ClusterAggregateTest, RetryOnSnapshotError) {
    testRetryOnSnapshotError(kAggregateCmdTargeted, kAggregateCmdScatterGather);
}

TEST_F(ClusterAggregateTest, MaxRetriesSnapshotErrors) {
    testMaxRetriesSnapshotErrors(kAggregateCmdTargeted, kAggregateCmdScatterGather);
}

TEST_F(ClusterAggregateTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    testAttachesAtClusterTimeForSnapshotReadConcern(kAggregateCmdTargeted,
                                                    kAggregateCmdScatterGather);
}

TEST_F(ClusterAggregateTest, SnapshotReadConcernWithAfterClusterTime) {
    testSnapshotReadConcernWithAfterClusterTime(kAggregateCmdTargeted, kAggregateCmdScatterGather);
}

TEST_F(ClusterAggregateTest, ShouldFailWhenFromRouterIsTrue) {
    const BSONObj inputBson =
        fromjson("{aggregate: 'coll', pipeline: [], cursor: {}, fromRouter: true, $db: 'test'}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51089);
}

TEST_F(ClusterAggregateTest, ShouldFailWhenNeedsMergeIstrueAndFromRouterIsFalse) {
    const BSONObj inputBson = fromjson(
        "{aggregate: 'coll', pipeline: [], cursor: {}, needsMerge: true, fromRouter: false, $db: "
        "'test'}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51089);
}

TEST_F(ClusterAggregateTest, ShouldFailWhenNeedsMergeIstrueAndFromRouterIsTrue) {
    const BSONObj inputBson = fromjson(
        "{aggregate: 'coll', pipeline: [], cursor: {}, needsMerge: true, fromRouter: true, $db: "
        "'test'}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51089);
}

TEST_F(ClusterAggregateTest, ShouldFailWhenExchengeIsPresent) {
    const BSONObj inputBson = fromjson(
        "{aggregate: 'coll', pipeline: [], cursor: {}, exchange: {policy: 'roundrobin', consumers: "
        "NumberInt(2)}, $db: 'test'}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51028);
}

/**
 * A test fixture for making aggregate() requests followed by getMore()s, where we can examine
 * memory metrics between requests.
 */
class ClusterAggregateMemoryTrackingTest : public ClusterCommandTestFixture {
public:
    void setUp() override {
        ClusterCommandTestFixture::setUp();
        // Use the same logical session ID for all requests.
        _lsid = makeLogicalSessionIdForTest();
    }

    void tearDown() override {
        _lsid = boost::none;
        ClusterCommandTestFixture::tearDown();
    }

protected:
    /**
     * Queue up a response that the router will receive if it makes a request to a shard.
     */
    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            return makeShardResponse(shardIndex, request);
        });
    }

    /**
     * This method is very similar to the above but requires a callback to be supplied. It's not
     * used for this test, but an implementation is required by the base class.
     */
    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor(
            [this, cb, shardIndex](const executor::RemoteCommandRequest& request) {
                cb(request);
                return makeShardResponse(shardIndex, request);
            });
    }

    /**
     * Run a command against a router that invokes a request against all shards. Return the result
     * as BSON. Code in the base class will run expectReturnsSuccess() to queue up the responses.
     */
    BSONObj runScatterGatherCmd(BSONObj cmdObj) {
        const bool isTargeted = false;
        auto response = runCommandSuccessful(_makeCmd(cmdObj), isTargeted);
        return rpc::makeReply(&response.response)->getCommandReply().getOwned();
    }

    /**
     * Run a command that doesn't cause the router to contact any shards. Note: the test will hang
     * if the router issues a request to a shard when no responses are queued, or if the test queues
     * a shard response that is never consumed.
     */
    BSONObj runRouterOnlyCmd(BSONObj cmdObj) {
        DbResponse response = runCommand(_makeCmd(cmdObj));
        return rpc::makeReply(&response.response)->getCommandReply().getOwned();
    }

    /**
     * After an aggregate() request has been issued but its cursor hasn't been exhausted, get the
     * memory tracker stashed on the cursor and return the metrics that it contains.
     *
     * Returns a tuple of (currentMemoryBytes, maxMemoryBytes).
     */
    std::pair<int64_t, int64_t> getRouterMemoryUsage(int64_t cursorId) {
        auto client = getServiceContext()->getService()->makeClient("ClusterCmdClient");
        auto opCtx = client->makeOperationContext();
        auto cursorManager = Grid::get(&*opCtx)->getCursorManager();
        auto pinnedCursor =
            unittest::assertGet(cursorManager->checkOutCursorNoAuthCheck(cursorId, &*opCtx));
        // Make sure to return the cursor when we're done.
        ScopeGuard guard{[&] {
            pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
        }};
        OperationMemoryUsageTracker* memoryUsageTracker =
            OperationMemoryUsageTracker::getFromClientCursor_forTest(pinnedCursor.operator->());
        ASSERT_NE(nullptr, memoryUsageTracker);
        return std::make_pair(memoryUsageTracker->currentMemoryBytes(),
                              memoryUsageTracker->maxMemoryBytes());
    }

private:
    /**
     * Make a response that the router will receive when it makes a request to a shard.
     */
    BSONObj makeShardResponse(int shardIndex, const executor::RemoteCommandRequest& request) {
        // There will be commands issued from the router to the shards with the shard part of the
        // split pipeline.
        ASSERT_EQ(request.cmdObj.firstElementFieldNameStringData(), "aggregate");
        ASSERT_EQ(request.cmdObj.firstElement().String(), "coll");
        BSONObjBuilder bob;
        // Shard 0 will have _ids:
        //   [0, 1, 2, 6, 7, 8]
        // Shard 1 will have _ids:
        //   [3, 4, 5, 9, 10, 11]
        int32_t v0 = shardIndex * 3, v1 = (shardIndex + 2) * 3;
        std::vector<BSONObj> batch = {BSON("_id" << 0 << "pushed" << BSON_ARRAY(v0 << v1)),
                                      BSON("_id" << 1 << "pushed" << BSON_ARRAY(v0 + 1 << v1 + 1)),
                                      BSON("_id" << 2 << "pushed" << BSON_ARRAY(v0 + 2 << v1 + 2))};
        // CursorId of zero means that there is no more data from this shard.
        CursorResponse cursorResponse(kNss, CursorId(0), batch);

        const auto cursorResponseObj =
            cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        bob.appendElementsUnique(cursorResponseObj);

        appendTxnResponseMetadata(bob);
        return bob.obj();
    }

    BSONObj _makeCmd(BSONObj cmdObj) {
        BSONObjBuilder bob(cmdObj);
        bob.append("lsid", _lsid->toBSON());
        // All requests must use the same transaction number. The cursor created by the aggregate()
        // command is only accessible from the same transaction.
        bob.append("txnNumber", TxnNumber(1));
        bob.append("autocommit", false);
        if (cmdObj.firstElementFieldNameStringData() != "getMore") {
            bob.append("startTransaction", true);
            BSONObjBuilder readConcernBob =
                bob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
            readConcernBob.append("level", "snapshot");
            readConcernBob.doneFast();
        }

        return bob.obj();
    }

    boost::optional<LogicalSessionId> _lsid;
};

TEST_F(ClusterAggregateMemoryTrackingTest, MemoryTrackingWorksOnRouter) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);
    // Force the classic engine so we can use the classic $group stage.
    RAIIServerParameterControllerForTest paramController("internalQueryFrameworkControl",
                                                         "forceClassicEngine");
    // Run a query that will produce three documents, with a batch size of 1, so that we will need
    // to call getMore().
    BSONObj groupCmdObj = fromjson(R"({
        aggregate: 'coll', 
        pipeline: [
            {$group: {_id: {$mod: ["$_id", 3]}, pushed: {$push: "$_id"}}}
        ],
        allowDiskUse: false, 
        cursor: {batchSize: 1}
    })");

    BSONObj res = runScatterGatherCmd(groupCmdObj);
    ASSERT_EQ(res["ok"].Number(), 1.0);
    int64_t cursorId = res["cursor"].Obj()["id"].Long();
    int64_t prevMaxMemoryInUse = 0;

    while (cursorId != 0) {
        // The max memory used thus far should only increase.
        int64_t currentMemoryInUse, currentMaxMemoryInUse;
        std::tie(currentMemoryInUse, currentMaxMemoryInUse) = getRouterMemoryUsage(cursorId);
        ASSERT_GT(currentMemoryInUse, 0);
        ASSERT_GTE(currentMaxMemoryInUse, prevMaxMemoryInUse);
        ASSERT_GTE(currentMaxMemoryInUse, currentMemoryInUse);
        prevMaxMemoryInUse = currentMaxMemoryInUse;

        // There is still a valid cursor, so get the next batch. The mocked shards return all their
        // data during the initial request, so this command runs only on the router.
        BSONObj getMoreCmdObj = fromjson(fmt::format(
            R"({{
                getMore: {},
                collection: "coll",
                batchSize: 1
            }})",
            cursorId));
        res = runRouterOnlyCmd(getMoreCmdObj);
        ASSERT_EQ(res["ok"].Number(), 1.0);
        cursorId = res["cursor"].Obj()["id"].Long();
    }
}

}  // namespace
}  // namespace mongo
