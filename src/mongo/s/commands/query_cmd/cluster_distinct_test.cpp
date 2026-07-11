// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/cluster_command_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"

#include <functional>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

class ClusterDistinctTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kDistinctCmdTargeted{
        fromjson("{distinct: 'coll', key: 'x', query: {'_id': {$lt: -1}}}")};

    const BSONObj kDistinctCmdScatterGather{fromjson("{distinct: 'coll', key: '_id'}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            cb(request);

            BSONObjBuilder bob;
            bob.append("values", BSON_ARRAY(shardIndex));
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            BSONObjBuilder bob;
            bob.append("values", BSON_ARRAY(shardIndex));
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterDistinctTest, NoErrors) {
    testNoErrors(kDistinctCmdTargeted, kDistinctCmdScatterGather);
}

TEST_F(ClusterDistinctTest, RetryOnSnapshotError) {
    testRetryOnSnapshotError(kDistinctCmdTargeted, kDistinctCmdScatterGather);
}

TEST_F(ClusterDistinctTest, MaxRetriesSnapshotErrors) {
    testMaxRetriesSnapshotErrors(kDistinctCmdTargeted, kDistinctCmdScatterGather);
}

TEST_F(ClusterDistinctTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    testAttachesAtClusterTimeForSnapshotReadConcern(kDistinctCmdTargeted,
                                                    kDistinctCmdScatterGather);
}

TEST_F(ClusterDistinctTest, SnapshotReadConcernWithAfterClusterTime) {
    testSnapshotReadConcernWithAfterClusterTime(kDistinctCmdTargeted, kDistinctCmdScatterGather);
}

}  // namespace
}  // namespace mongo
