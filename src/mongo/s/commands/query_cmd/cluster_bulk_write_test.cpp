// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/sharding_environment/cluster_command_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"

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
    unittest::ServerParameterGuard controller("featureFlagBulkWriteCommand", true);
    unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor", false);

    testNoErrors(kBulkWriteCmdTargeted, kBulkWriteCmdScatterGather);
}

TEST_F(ClusterBulkWriteTest, AttachesAtClusterTimeForSnapshotReadConcern) {
    unittest::ServerParameterGuard controller("featureFlagBulkWriteCommand", true);
    unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor", false);

    testAttachesAtClusterTimeForSnapshotReadConcern(kBulkWriteCmdTargeted,
                                                    kBulkWriteCmdScatterGather);
}

TEST_F(ClusterBulkWriteTest, SnapshotReadConcernWithAfterClusterTime) {
    unittest::ServerParameterGuard controller("featureFlagBulkWriteCommand", true);
    unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor", false);

    testSnapshotReadConcernWithAfterClusterTime(kBulkWriteCmdTargeted, kBulkWriteCmdScatterGather);
}

TEST_F(ClusterBulkWriteTest, FireAndForgetRequestGetsReplyWithOnlyOkStatus) {
    unittest::ServerParameterGuard controller("featureFlagBulkWriteCommand", true);
    unittest::ServerParameterGuard uweController("featureFlagUnifiedWriteExecutor", false);

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
