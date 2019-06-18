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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/query/cursor_response.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/util/log.h"

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
    void expectReturnsSuccess(int shardIndex) {
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

    void expectInspectRequest(int shardIndex, InspectionCallback cb) {
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
        NamespaceString nss{"a.collection"};
        auto client = getServiceContext()->makeClient("ClusterCmdClient");
        auto opCtx = client->makeOperationContext();
        auto request = AggregationRequest::parseFromBSON(nss, inputBson);
        if (request.getStatus() != Status::OK()) {
            return request.getStatus();
        }
        return ClusterAggregate::runAggregate(opCtx.get(),
                                              ClusterAggregate::Namespaces{nss, nss},
                                              request.getValue(),
                                              PrivilegeVector(),
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

TEST_F(ClusterAggregateTest, ShouldFailWhenFromMongosIsTrue) {
    const BSONObj inputBson = fromjson("{pipeline: [], cursor: {}, fromMongos: true}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51089);
}

TEST_F(ClusterAggregateTest, ShouldFailWhenNeedsMergeIstrueAndFromMongosIsFalse) {
    const BSONObj inputBson =
        fromjson("{pipeline: [], cursor: {}, needsMerge: true, fromMongos: false}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51089);
}

TEST_F(ClusterAggregateTest, ShouldFailWhenNeedsMergeIstrueAndFromMongosIsTrue) {
    const BSONObj inputBson =
        fromjson("{pipeline: [], cursor: {}, needsMerge: true, fromMongos: true}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51089);
}

TEST_F(ClusterAggregateTest, ShouldFailWhenExchengeIsPresent) {
    const BSONObj inputBson = fromjson(
        "{pipeline: [], cursor: {}, exchange: {policy: 'roundrobin', consumers: NumberInt(2)}}");
    ASSERT_THROWS_CODE(testRunAggregateEarlyExit(inputBson), AssertionException, 51028);
}

}  // namespace
}  // namespace mongo
