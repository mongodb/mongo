// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/sharding_environment/cluster_command_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

class ClusterFindTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kFindCmdScatterGather = BSON("find" << "coll");
    const BSONObj kFindCmdTargeted = BSON("find" << "coll"
                                                 << "filter" << BSON("_id" << 0));

    // The index of the shard expected to receive the response is used to prevent different shards
    // from returning documents with the same shard key. This is expected to be 0 for queries
    // targeting one shard.
    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            std::vector<BSONObj> batch = {BSON("_id" << shardIndex)};
            CursorResponse cursorResponse(kNss, CursorId(0), batch);

            BSONObjBuilder bob;
            bob.appendElementsUnique(
                cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse));
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

TEST_F(ClusterFindTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    testAttachesAtClusterTimeForSnapshotReadConcern(kFindCmdTargeted, kFindCmdScatterGather);
}

TEST_F(ClusterFindTest, SnapshotReadConcernWithAfterClusterTime) {
    testSnapshotReadConcernWithAfterClusterTime(kFindCmdTargeted, kFindCmdScatterGather);
}

TEST_F(ClusterFindTest, IncludeQueryStatsMetrics) {
    testIncludeQueryStatsMetrics(kFindCmdTargeted, true /* isTargeted */);
    testIncludeQueryStatsMetrics(kFindCmdScatterGather, false /* isTargeted */);
}

}  // namespace
}  // namespace mongo
