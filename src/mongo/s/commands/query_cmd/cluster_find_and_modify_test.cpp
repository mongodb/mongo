// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonelement.h"
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

class ClusterFindAndModifyTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kFindAndModifyCmdTargeted{
        fromjson("{findAndModify: 'coll', query: {'_id': -1}, update: {$set: {x: 1}}}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            cb(request);

            BSONObjBuilder bob(BSON("value" << BSONNULL << "lastErrorObject"
                                            << BSON("n" << 0 << "updatedExisting" << false)));
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            BSONObjBuilder bob(BSON("value" << BSONNULL << "lastErrorObject"
                                            << BSON("n" << 0 << "updatedExisting" << false)));
            bob.append("_id", -1);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterFindAndModifyTest, NoErrors) {
    testNoErrors(kFindAndModifyCmdTargeted);
}

TEST_F(ClusterFindAndModifyTest, RetryOnSnapshotError) {
    testRetryOnSnapshotError(kFindAndModifyCmdTargeted);
}

TEST_F(ClusterFindAndModifyTest, MaxRetriesSnapshotErrors) {
    testMaxRetriesSnapshotErrors(kFindAndModifyCmdTargeted);
}

TEST_F(ClusterFindAndModifyTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    testAttachesAtClusterTimeForSnapshotReadConcern(kFindAndModifyCmdTargeted);
}

TEST_F(ClusterFindAndModifyTest, SnapshotReadConcernWithAfterClusterTime) {
    testSnapshotReadConcernWithAfterClusterTime(kFindAndModifyCmdTargeted);
}

}  // namespace
}  // namespace mongo
