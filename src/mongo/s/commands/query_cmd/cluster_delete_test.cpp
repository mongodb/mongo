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


#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"
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
    testNoErrors(kDeleteCmdTargeted, kDeleteCmdScatterGather);
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

    testOpcountersAreCorrect(kDeleteCmdTargeted, /* expectedValue */ b.obj());
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
