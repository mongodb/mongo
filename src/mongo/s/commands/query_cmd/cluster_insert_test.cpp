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


#include <functional>

#include <boost/move/utility_core.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

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
    testNoErrors(kInsertCmdTargeted, kInsertCmdScatterGather);
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

    testOpcountersAreCorrect(kInsertCmdTargeted, /* expectedValue */ b.obj());
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

    testOpcountersAreCorrect(bulkInsertCmd, /* expectedValue */ b.obj());
}

}  // namespace
}  // namespace mongo
