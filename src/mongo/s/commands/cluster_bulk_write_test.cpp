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

#include "mongo/bson/json.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

class ClusterBulkWriteTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kBulkWriteCmdTargeted{fromjson(
        "{bulkWrite: 1, ops: [{insert: 0, document: {'_id': -1}}], nsInfo: [{ns: 'test.coll'}]}")};

    const BSONObj kBulkWriteCmdScatterGather{
        fromjson("{bulkWrite: 1, ops: [{insert: 0, document: {'_id': -1}}, {insert: 0, document: "
                 "{'_id': 1}}], nsInfo: [{ns: 'test.coll'}]}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(1, request.cmdObj.firstElement().Int());
            cb(request);

            BSONObjBuilder bob;
            std::vector<BulkWriteReplyItem> replyItems;
            BulkWriteReplyItem item{0};
            item.setN(1);
            replyItems.push_back(item);
            auto cursor = BulkWriteCommandResponseCursor(
                0, replyItems, NamespaceString::makeBulkWriteNSS(boost::none));
            bob.append("cursor", cursor.toBSON());
            bob.append("nErrors", 0);
            bob.append("nInserted", 0);
            bob.append("nDeleted", 0);
            bob.append("nMatched", 0);
            bob.append("nModified", 0);
            bob.append("nUpserted", 0);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(1, request.cmdObj.firstElement().Int());

            BSONObjBuilder bob;
            std::vector<BulkWriteReplyItem> replyItems;
            BulkWriteReplyItem item{0};
            item.setN(1);
            replyItems.push_back(item);
            auto cursor = BulkWriteCommandResponseCursor(
                0, replyItems, NamespaceString::makeBulkWriteNSS(boost::none));
            bob.append("cursor", cursor.toBSON());
            bob.append("nErrors", 0);
            bob.append("nInserted", 0);
            bob.append("nDeleted", 0);
            bob.append("nMatched", 0);
            bob.append("nModified", 0);
            bob.append("nUpserted", 0);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterBulkWriteTest, NoErrors) {
    RAIIServerParameterControllerForTest controller("featureFlagBulkWriteCommand", true);
    testNoErrors(kBulkWriteCmdTargeted, kBulkWriteCmdScatterGather);
}

TEST_F(ClusterBulkWriteTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    RAIIServerParameterControllerForTest controller("featureFlagBulkWriteCommand", true);
    testAttachesAtClusterTimeForSnapshotReadConcern(kBulkWriteCmdTargeted,
                                                    kBulkWriteCmdScatterGather);
}

TEST_F(ClusterBulkWriteTest, SnapshotReadConcernWithAfterClusterTime) {
    RAIIServerParameterControllerForTest controller("featureFlagBulkWriteCommand", true);
    testSnapshotReadConcernWithAfterClusterTime(kBulkWriteCmdTargeted, kBulkWriteCmdScatterGather);
}

TEST_F(ClusterBulkWriteTest, FireAndForgetRequestGetsReplyWithOnlyOkStatus) {
    RAIIServerParameterControllerForTest controller("featureFlagBulkWriteCommand", true);

    auto asFireAndForgetRequest = [](const BSONObj& cmdObj) {
        BSONObjBuilder bob(cmdObj);
        bob.append("lsid", makeLogicalSessionIdForTest().toBSON());
        bob.append("txnNumber", TxnNumber(1));
        bob.append(WriteConcernOptions::kWriteConcernField, WriteConcernOptions::Unacknowledged);
        bob.doneFast();
        return bob.obj();
    };

    const auto bulkCmdresponses =
        testNoErrorsOutsideTransaction(asFireAndForgetRequest(kBulkWriteCmdTargeted),
                                       asFireAndForgetRequest(kBulkWriteCmdScatterGather));

    // A "fire & forget" bulk write is replied with the expected schema, but no meaningful value.
    const auto expectedBulkCommandReplySection =
        BulkWriteCommandReply(
            BulkWriteCommandResponseCursor(
                0 /* cursorId */,
                {} /* firstBatch */,
                NamespaceString::createNamespaceString_forTest("admin.$cmd.bulkWrite")),
            0 /*nErrors*/,
            0 /*nInserted*/,
            0 /*nMatched*/,
            0 /*nModified*/,
            0 /*nUpserted*/,
            0 /*nDeleted*/)
            .toBSON();
    for (const auto& response : bulkCmdresponses) {
        ASSERT_FALSE(response.shouldRunAgainForExhaust);
        ASSERT(!response.nextInvocation);
        const auto responseObj = OpMsgRequest::parse(response.response).body;
        ASSERT_EQ(1, responseObj.getIntField("ok"));
        ASSERT_TRUE(expectedBulkCommandReplySection.isPrefixOf(
            responseObj, SimpleBSONElementComparator::kInstance));
    }
}

}  // namespace
}  // namespace mongo
