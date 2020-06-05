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
#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/s/query/cluster_cursor_manager.h"

namespace mongo {
namespace {

class ClusterFindTest : public ClusterCommandTestFixture {
protected:
    // Batch size 1, so when expectInspectRequest returns one doc, mongos doesn't wait for more.
    const BSONObj kFindCmdScatterGather = BSON("find" << kNss.coll() << "batchSize" << 1);
    const BSONObj kFindCmdTargeted =
        BSON("find" << kNss.coll() << "filter" << BSON("_id" << 0) << "batchSize" << 1);

    // The index of the shard expected to receive the response is used to prevent different shards
    // from returning documents with the same shard key. This is expected to be 0 for queries
    // targeting one shard.
    void expectReturnsSuccess(int shardIndex) override {
        expectInspectRequest(shardIndex, [](const executor::RemoteCommandRequest&) {});
    }

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            cb(request);

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            // User supplies no atClusterTime. For single-shard non-transaction snapshot reads,
            // mongos lets the shard (which this function simulates) select a read timestamp.
            boost::optional<Timestamp> atClusterTime;
            if (request.cmdObj["txnNumber"].eoo() && !request.cmdObj["readConcern"].eoo()) {
                auto rc = request.cmdObj["readConcern"].Obj();
                if (rc["level"].String() == "snapshot" && rc["atClusterTime"].eoo()) {
                    atClusterTime = kShardClusterTime;
                }
            }

            CursorResponse cursorResponse(kNss, kCursorId, batch, atClusterTime);
            BSONObjBuilder bob;
            bob.appendElementsUnique(
                cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse));
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterFindTest, NoErrors) {
    testNoErrors(kFindCmdTargeted, kFindCmdScatterGather);
}

TEST_F(ClusterFindTest, RetryOnSnapshotError) {
    testRetryOnSnapshotError(kFindCmdTargeted, kFindCmdScatterGather);
}

TEST_F(ClusterFindTest, MaxRetriesSnapshotErrors) {
    testMaxRetriesSnapshotErrors(kFindCmdTargeted, kFindCmdScatterGather);
}

TEST_F(ClusterFindTest, AttachesAtClusterTimeForTransactionSnapshotReadConcern) {
    testAttachesAtClusterTimeForTxnSnapshotReadConcern(
        kFindCmdTargeted, kFindCmdScatterGather, true);
}

TEST_F(ClusterFindTest, TransactionSnapshotReadConcernWithAfterClusterTime) {
    testTxnSnapshotReadConcernWithAfterClusterTime(kFindCmdTargeted, kFindCmdScatterGather, true);
}

TEST_F(ClusterFindTest, AttachesAtClusterTimeForNonTransactionSnapshotReadConcern) {
    testAttachesAtClusterTimeForNonTxnSnapshotReadConcern(
        kFindCmdTargeted, kFindCmdScatterGather, true);
}

TEST_F(ClusterFindTest, NonTransactionSnapshotReadConcernWithAfterClusterTime) {
    testNonTxnSnapshotReadConcernWithAfterClusterTime(
        kFindCmdTargeted, kFindCmdScatterGather, true);
}

}  // namespace
}  // namespace mongo
