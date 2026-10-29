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

class ClusterInsertTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kInsertCmdTargeted{fromjson("{insert: 'coll', documents: [{'_id': -1}]}")};

    const BSONObj kInsertCmdScatterGather{
        fromjson("{insert: 'coll', documents: [{'_id': -1}, {'_id': 1}], ordered: false}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            cb(request);

            BSONObjBuilder bob;
            bob.append("nInserted", 1);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            BSONObjBuilder bob;
            bob.append("nInserted", 1);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterInsertTest, NoErrors) {
    for (auto uweKnobValue : {false, true}) {
        unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor",
                                                     uweKnobValue);
        testNoErrors(kInsertCmdTargeted, kInsertCmdScatterGather);
    }
}

TEST_F(ClusterInsertTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    testAttachesAtClusterTimeForSnapshotReadConcern(kInsertCmdTargeted, kInsertCmdScatterGather);
}

TEST_F(ClusterInsertTest, SnapshotReadConcernWithAfterClusterTime) {
    testSnapshotReadConcernWithAfterClusterTime(kInsertCmdTargeted, kInsertCmdScatterGather);
}

TEST_F(ClusterInsertTest, CorrectMetricsSingleInsert) {
    BSONObjBuilder b;
    b.append("insert", 1);
    b.append("query", 0);
    b.append("update", 0);
    b.append("delete", 0);
    b.append("getmore", 0);
    b.append("command", 0);

    const BSONObj obj = b.obj();

    for (auto uweKnobValue : {false, true}) {
        unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor",
                                                     uweKnobValue);
        testOpcountersAreCorrect(kInsertCmdTargeted, /* expectedValue */ obj);
    }
}

TEST_F(ClusterInsertTest, CorrectMetricsBulkInsert) {
    BSONObjBuilder b;
    b.append("insert", 2);
    b.append("query", 0);
    b.append("update", 0);
    b.append("delete", 0);
    b.append("getmore", 0);
    b.append("command", 0);

    const BSONObj bulkInsertCmd{
        fromjson("{insert: 'coll', documents: [{'_id': -1}, {'_id': -2}]}")};
    const BSONObj obj = b.obj();

    for (auto uweKnobValue : {false, true}) {
        unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor",
                                                     uweKnobValue);
        testOpcountersAreCorrect(bulkInsertCmd, /* expectedValue */ obj);
    }
}

TEST_F(ClusterInsertTest, RejectsCmdAggregateNamespace) {
    auto opCtx = operationContext();
    const auto badInsert = fromjson("{insert: '$cmd.aggregate', documents: [{'_id': 1}]}");
    auto req = makeRequest(kNss, badInsert);
    auto insertCmd = CommandHelpers::findCommand(opCtx, "insert");

    ASSERT_THROWS_CODE(
        insertCmd->parse(opCtx, req), AssertionException, ErrorCodes::InvalidNamespace);
}

}  // namespace
}  // namespace mongo
