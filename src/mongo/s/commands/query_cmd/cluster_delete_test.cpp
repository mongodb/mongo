// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/cluster_command_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <functional>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

class ClusterDeleteTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kDeleteCmdTargeted{
        fromjson("{delete: 'coll', deletes: [{q: {'_id': -1}, limit: 0}]}")};

    const BSONObj kDeleteCmdScatterGather{
        fromjson("{delete: 'coll', deletes: [{q: {'_id': {$gte: -1}}, limit: 0}]}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            cb(request);

            BSONObjBuilder bob;
            bob.append("n", 1);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            BSONObjBuilder bob;
            bob.append("n", 1);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterDeleteTest, NoErrors) {
    for (auto uweKnobValue : {false, true}) {
        unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor",
                                                     uweKnobValue);
        testNoErrors(kDeleteCmdTargeted, kDeleteCmdScatterGather);
    }
}

TEST_F(ClusterDeleteTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    testAttachesAtClusterTimeForSnapshotReadConcern(kDeleteCmdTargeted, kDeleteCmdScatterGather);
}

TEST_F(ClusterDeleteTest, SnapshotReadConcernWithAfterClusterTime) {
    testSnapshotReadConcernWithAfterClusterTime(kDeleteCmdTargeted, kDeleteCmdScatterGather);
}

TEST_F(ClusterDeleteTest, CorrectMetrics) {
    BSONObjBuilder b;
    b.append("insert", 0);
    b.append("query", 0);
    b.append("update", 0);
    b.append("delete", 1);
    b.append("getmore", 0);
    b.append("command", 0);
    const BSONObj obj = b.obj();
    for (auto uweKnobValue : {false, true}) {
        unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor",
                                                     uweKnobValue);
        testOpcountersAreCorrect(kDeleteCmdTargeted, /* expectedValue */ obj);
    }
}

TEST_F(ClusterDeleteTest, RejectsCmdAggregateNamespace) {
    auto opCtx = operationContext();
    const auto badDelete =
        fromjson("{delete: '$cmd.aggregate', deletes: [{'q': {'_id': 1}, limit: 0}]}");
    auto req = makeRequest(kNss, badDelete);
    auto deleteCmd = CommandHelpers::findCommand(opCtx, "delete");

    ASSERT_THROWS_CODE(
        deleteCmd->parse(opCtx, req), AssertionException, ErrorCodes::InvalidNamespace);
}

}  // namespace
}  // namespace mongo
