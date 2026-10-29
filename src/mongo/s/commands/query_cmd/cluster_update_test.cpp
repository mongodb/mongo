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

class ClusterUpdateTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kUpdateCmdTargeted{
        fromjson("{update: 'coll', updates: [{q: {'_id': -1}, u: {$set: {x: 1}}}]}")};

    const BSONObj kUpdateCmdScatterGather{fromjson(
        "{update: 'coll', updates: [{q: {'_id': {$gte: -1}}, u: {$set: {x: 1}}, multi: true}]}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            cb(request);

            BSONObjBuilder bob;
            bob.append("nMatched", 1);
            bob.append("nUpserted", 0);
            bob.append("nModified", 1);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            BSONObjBuilder bob;
            bob.append("nMatched", 1);
            bob.append("nUpserted", 0);
            bob.append("nModified", 1);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterUpdateTest, NoErrors) {
    for (auto uweKnobValue : {false, true}) {
        unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor",
                                                     uweKnobValue);
        testNoErrors(kUpdateCmdTargeted, kUpdateCmdScatterGather);
    }
}

TEST_F(ClusterUpdateTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    testAttachesAtClusterTimeForSnapshotReadConcern(kUpdateCmdTargeted, kUpdateCmdScatterGather);
}

TEST_F(ClusterUpdateTest, SnapshotReadConcernWithAfterClusterTime) {
    testSnapshotReadConcernWithAfterClusterTime(kUpdateCmdTargeted, kUpdateCmdScatterGather);
}

TEST_F(ClusterUpdateTest, CorrectMetrics) {
    BSONObjBuilder b;
    b.append("insert", 0);
    b.append("query", 0);
    b.append("update", 1);
    b.append("delete", 0);
    b.append("getmore", 0);
    b.append("command", 0);

    const BSONObj obj = b.obj();
    for (auto uweKnobValue : {false, true}) {
        unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor",
                                                     uweKnobValue);
        testOpcountersAreCorrect(kUpdateCmdTargeted, /* expectedValue */ obj);
    }
}

TEST_F(ClusterUpdateTest, RejectsCmdAggregateNamespace) {
    auto opCtx = operationContext();
    const auto badUpdate = fromjson(
        "{ update: '$cmd.aggregate',"
        "  updates: [ { q: { _id: -1 }, u: { $set: { x: 1 } } } ]"
        "}");
    auto req = makeRequest(kNss, badUpdate);
    auto updateCmd = CommandHelpers::findCommand(opCtx, "update");

    ASSERT_THROWS_CODE(
        updateCmd->parse(opCtx, req), AssertionException, ErrorCodes::InvalidNamespace);
}

}  // namespace
}  // namespace mongo
