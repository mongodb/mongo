/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/unified_write_executor/write_batch_response_processor.h"

#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/catalog_cache_mock.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/unittest/unittest.h"

namespace mongo::unified_write_executor {
namespace {

class WriteBatchResponseProcessorTest : public unittest::Test {
public:
    using RemoteCommandResponse = executor::RemoteCommandResponse;
    using Response = ShardResponse;

    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test", "coll2");
    const NamespaceString nss3 = NamespaceString::createNamespaceString_forTest("test", "coll3");
    const HostAndPort host1 = HostAndPort("host1", 0);
    const HostAndPort host2 = HostAndPort("host2", 0);
    const ShardId shard1Name = ShardId("shard1");
    const ShardId shard2Name = ShardId("shard2");
    // This is a class because OID::gen() relies on initialization of static variables.
    const CollectionGeneration gen{OID::gen(), Timestamp(1, 1)};
    const ShardEndpoint shard1Endpoint = ShardEndpoint(
        shard1Name, ShardVersionFactory::make(ChunkVersion(gen, {100, 200})), boost::none);
    const ShardEndpoint shard2Endpoint = ShardEndpoint(
        shard2Name, ShardVersionFactory::make(ChunkVersion(gen, {100, 200})), boost::none);


    BulkWriteCommandReply makeReply() {
        return BulkWriteCommandReply(BulkWriteCommandResponseCursor(0, {}, nss1), 0, 0, 0, 0, 0, 0);
    }

    BSONObj setTopLevelOK(BSONObj&& o) {
        return o.addFields(BSON("ok" << 1));
    }
};

TEST_F(WriteBatchResponseProcessorTest, OKReplies) {
    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(0, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1)});
    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNMatched(3);
    reply.setNModified(3);
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);

    processor.onWriteBatchResponse(
        {{shard1Name, Response{rcr1, {}}},
         {shard2Name, Response{rcr2, {WriteOp(request, 0), WriteOp(request, 1)}}}});

    auto clientReply = processor.generateClientResponse<BulkWriteCommandReply>();
    ASSERT_EQ(clientReply.getNInserted(), 2);
    ASSERT_EQ(clientReply.getNMatched(), 6);
    ASSERT_EQ(clientReply.getNModified(), 6);

    // Generating a 'BatchedCommandResponse' should output the same statistics, save for 'n', which
    // is the combination of 'nInserted' and 'nMatched'.
    auto batchedCommandReply = processor.generateClientResponse<BatchedCommandResponse>();
    ASSERT_EQ(batchedCommandReply.getN(), 8);
    ASSERT_EQ(batchedCommandReply.getNModified(), 6);
}

TEST_F(WriteBatchResponseProcessorTest, AllStatisticsCopied) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1))},
                                           {NamespaceInfoEntry(nss1)});
    auto reply = BulkWriteCommandReply(
        BulkWriteCommandResponseCursor(
            0, {BulkWriteReplyItem{0, Status(ErrorCodes::BadValue, "")}}, nss1),
        1,
        1,
        1,
        1,
        1,
        1);
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);

    processor.onWriteBatchResponse({{shard1Name, Response{rcr1, {WriteOp(request, 0)}}}});
    auto clientReply = processor.generateClientResponse<BulkWriteCommandReply>();
    ASSERT_EQ(clientReply.getNInserted(), 1);
    ASSERT_EQ(clientReply.getNMatched(), 1);
    ASSERT_EQ(clientReply.getNModified(), 1);
    ASSERT_EQ(clientReply.getNUpserted(), 1);
    ASSERT_EQ(clientReply.getNDeleted(), 1);

    // Only tracks no retriable errors from reply items not the summary from the shard reply.
    ASSERT_EQ(clientReply.getNErrors(), 1);
}

TEST_F(WriteBatchResponseProcessorTest, MixedErrorsAndOk) {
    // shard1: {code: BadValue}
    // shard2: {code: ok, firstBatch: [{code: BadValue}, {code: ok}]}
    // shard3: {code: ok, firstBatch: [{code: ok}]}

    // This test doesn't represent a real sequence of what could happen with the following command,
    // its just for testing to pass around a write op.
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(0, BSON("_id" << 2)),
                                            BulkWriteInsertOp(0, BSON("_id" << 3)),
                                            BulkWriteInsertOp(0, BSON("_id" << 4))},
                                           {NamespaceInfoEntry(nss1)});
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);
    WriteOp op4(request, 3);

    ShardId shard3Name = ShardId("shard3");
    ShardEndpoint shard3Endpoint = ShardEndpoint(
        shard3Name, ShardVersionFactory::make(ChunkVersion(gen, {100, 200})), boost::none);
    HostAndPort host3 = HostAndPort("host3", 0);

    RemoteCommandResponse rcr1(
        host1,
        ErrorReply(false, ErrorCodes::BadValue, "Bad Value", "simulating error").toBSON(),
        Microseconds{0},
        false);
    auto reply = BulkWriteCommandReply(
        BulkWriteCommandResponseCursor(0,
                                       {BulkWriteReplyItem{0, Status(ErrorCodes::BadValue, "")},
                                        BulkWriteReplyItem{1, Status::OK()}},
                                       nss1),
        1,
        1,
        0,
        0,
        0,
        0);
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    auto reply2 = makeReply();
    reply2.setNInserted(1);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr3(host3, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);

    processor.onWriteBatchResponse({{shard1Name, Response{rcr1, {op1}}},
                                    {shard2Name, Response{rcr2, {op2, op3}}},
                                    {shard3Name, Response{rcr3, {op4}}}});


    auto clientReply = processor.generateClientResponse<BulkWriteCommandReply>();
    ASSERT_EQ(clientReply.getNErrors(), 2);
    // Should still be able to keep processing even if we encountered an inner error.
    ASSERT_EQ(clientReply.getNInserted(), 2);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 4);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[2].getIdx(), 2);
    ASSERT_EQ(batch[3].getIdx(), 3);
    ASSERT_EQ(batch[0].getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(batch[1].getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(batch[2].getStatus(), Status::OK());
    ASSERT_EQ(batch[3].getStatus(), Status::OK());
}

TEST_F(WriteBatchResponseProcessorTest, CreateCollection) {
    // shard1: {code: ok, firstBatch: [{code: ok}, {code: CannotImplicitlyCreateCollection}]}
    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(1, BSON("_id" << 1))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);

    auto reply = makeReply();
    reply.setNErrors(1);
    reply.setNInserted(1);
    reply.setCursor(BulkWriteCommandResponseCursor(
        0,
        {
            BulkWriteReplyItem{0, Status::OK()},
            BulkWriteReplyItem{1, Status(CannotImplicitlyCreateCollectionInfo(nss2), "")},
        },
        nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);
    auto result = processor.onWriteBatchResponse({{shard1Name,
                                                   Response{
                                                       rcr1,
                                                       {op1, op2},
                                                   }}});
    // No unrecoverable error.
    ASSERT_FALSE(result.unrecoverableError);
    // One incomplete returned (op2).
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.opsToRetry[0].getNss(), nss2);
    ASSERT_EQ(result.opsToRetry[0].getId(), 1);

    // Assert nss2 was flagged for creation.
    ASSERT_EQ(result.collsToCreate.size(), 1);
    ASSERT(result.collsToCreate.contains(nss2));

    // Confirm so far we've only processed one error. Copy the processor since generating a
    // response consumes the results array.
    auto tempProcessor = processor;
    auto clientReply = tempProcessor.generateClientResponse<BulkWriteCommandReply>();
    // Should have 0 errors since we can retry CannotImplicitlyCreateCollection
    ASSERT_EQ(clientReply.getNErrors(), 0);
    // Should still be able to keep processing even if we encountered an inner error.
    ASSERT_EQ(clientReply.getNInserted(), 1);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 1);
    auto res0 = batch[0];
    ASSERT_EQ(res0.getStatus(), Status::OK());

    // Simulate a successful retry.
    reply = makeReply();
    reply.setNInserted(1);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       BulkWriteReplyItem{0, Status::OK()},
                                                   },
                                                   nss2));

    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    result = processor.onWriteBatchResponse({{shard1Name, Response{rcr2, {op2}}}});
    ASSERT_FALSE(result.unrecoverableError);
    ASSERT(result.opsToRetry.empty());
    ASSERT(result.collsToCreate.empty());
    clientReply = processor.generateClientResponse<BulkWriteCommandReply>();

    // Assert we have both ops completed now.
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 2);
    res0 = batch[0];
    auto res1 = batch[1];
    ASSERT_EQ(res0.getStatus(), Status::OK());
    ASSERT_EQ(res1.getStatus(), Status::OK());
}

TEST_F(WriteBatchResponseProcessorTest, SingleReplyItemForBatchOfThree) {
    // shard1: {code: ok, firstBatch: [{code: CannotImplicitlyCreateCollection}]}
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(0, BSON("_id" << 2)),
                                            BulkWriteInsertOp(0, BSON("_id" << 3))},
                                           {NamespaceInfoEntry(nss1)});
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);

    auto reply = makeReply();
    reply.setNErrors(2);
    reply.setCursor(BulkWriteCommandResponseCursor(
        0, {BulkWriteReplyItem{0, Status(CannotImplicitlyCreateCollectionInfo(nss1), "")}}, nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);
    auto result = processor.onWriteBatchResponse({{shard1Name,
                                                   Response{
                                                       rcr1,
                                                       {op1, op2, op3},
                                                   }}});
    // No unrecoverable error.
    ASSERT_FALSE(result.unrecoverableError);
    // Assert all ops were returned for retry even though there was only one item in the reply.
    ASSERT_EQ(result.opsToRetry.size(), 3);
    ASSERT_EQ(result.opsToRetry[0].getId(), 0);
    ASSERT_EQ(result.opsToRetry[1].getId(), 1);
    ASSERT_EQ(result.opsToRetry[2].getId(), 2);
    // Assert nss1 was flagged for creation.
    ASSERT_EQ(result.collsToCreate.size(), 1);
    ASSERT(result.collsToCreate.contains(nss1));

    // Assert the generated response is as expected.
    auto response = processor.generateClientResponse<BulkWriteCommandReply>();
    // Should have 0 errors since we can retry CannotImplicitlyCreateCollection.
    ASSERT_EQ(response.getNErrors(), 0);
    ASSERT_EQ(response.getNInserted(), 0);
}

TEST_F(WriteBatchResponseProcessorTest, TwoShardMixedNamespaceExistence) {
    // shard1: {code: ok, firstBatch: [{code: ok}, {code: CannotImplicitlyCreateCollection}, {code:
    // CannotImplicitlyCreateCollection}]}
    // shard2: {code: ok, firstBatch: [{code: ok}, {code: CannotImplicitlyCreateCollection}, {code:
    // CannotImplicitlyCreateCollection}]}
    auto request = BulkWriteCommandRequest(
        {
            BulkWriteInsertOp(0, BSON("_id" << 1)),
            BulkWriteInsertOp(1, BSON("_id" << 2)),
            BulkWriteInsertOp(1, BSON("_id" << 3)),
            BulkWriteInsertOp(0, BSON("_id" << 4)),
            BulkWriteInsertOp(1, BSON("_id" << 5)),
            BulkWriteInsertOp(2, BSON("_id" << 6)),
        },
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2), NamespaceInfoEntry(nss3)});
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);
    WriteOp op4(request, 3);
    WriteOp op5(request, 4);
    WriteOp op6(request, 5);

    auto reply = makeReply();
    reply.setNErrors(2);
    reply.setCursor(BulkWriteCommandResponseCursor(
        0,
        {
            BulkWriteReplyItem{0, Status::OK()},
            BulkWriteReplyItem{1, Status(CannotImplicitlyCreateCollectionInfo(nss2), "")},
            BulkWriteReplyItem{2, Status(CannotImplicitlyCreateCollectionInfo(nss2), "")},
        },
        nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    auto reply2 = makeReply();
    reply2.setNInserted(1);
    reply2.setNErrors(2);
    reply2.setCursor(BulkWriteCommandResponseCursor(
        0,
        {BulkWriteReplyItem{0, Status::OK()},
         BulkWriteReplyItem{1, Status(CannotImplicitlyCreateCollectionInfo(nss2), "")},
         BulkWriteReplyItem{2, Status(CannotImplicitlyCreateCollectionInfo(nss3), "")}},
        nss1));
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);
    auto result = processor.onWriteBatchResponse({{shard1Name,
                                                   Response{
                                                       rcr1,
                                                       {op1, op2, op3},
                                                   }},
                                                  {shard2Name,
                                                   Response{
                                                       rcr2,
                                                       {op4, op5, op6},
                                                   }}});
    // No unrecoverable error.
    ASSERT_FALSE(result.unrecoverableError);
    // Assert all the errors were returned for retry.
    ASSERT_EQ(result.opsToRetry.size(), 4);
    ASSERT_EQ(result.opsToRetry[0].getId(), 1);
    ASSERT_EQ(result.opsToRetry[1].getId(), 2);
    ASSERT_EQ(result.opsToRetry[2].getId(), 4);
    ASSERT_EQ(result.opsToRetry[3].getId(), 5);
    // Assert nss2 and nss3 were flagged for creation.
    ASSERT_EQ(result.collsToCreate.size(), 2);
    ASSERT(result.collsToCreate.contains(nss2));
    ASSERT(result.collsToCreate.contains(nss3));
}

TEST_F(WriteBatchResponseProcessorTest, IdxsCorrectlyRewrittenInReplyItems) {
    // shard2: {code: ok, firstBatch: [{code: BadValue, originalId: 5, id: 0}, {code:
    // CannotImplicitlyCreateCollection, originalId: 0, id: 1}, {code: ok, originalId: 1, Id: 2}]}
    // shard1: {code: ok, firstBatch: [{code: BadValue, originalId: 4, id: 0}, {code: ok,
    // originalId: 3, id: 1}, {code: BadValue, originalId: 2, id: 2}]}
    auto request = BulkWriteCommandRequest(
        {
            BulkWriteInsertOp(0, BSON("_id" << 1)),
            BulkWriteInsertOp(1, BSON("_id" << 2)),
            BulkWriteInsertOp(1, BSON("_id" << 3)),
            BulkWriteInsertOp(0, BSON("_id" << 4)),
            BulkWriteInsertOp(1, BSON("_id" << 5)),
            BulkWriteInsertOp(2, BSON("_id" << 6)),
        },
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2), NamespaceInfoEntry(nss3)});
    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);  // shard2, id: 1, CannotImplicitlyCreateCollection
    WriteOp op2(request, 1);  // shard2, id: 2, OK
    WriteOp op3(request, 2);  // shard1, id: 1, OK
    WriteOp op4(request, 3);  // shard1, id: 2, BadValue
    WriteOp op5(request, 4);  // shard1, id: 0, BadValue
    WriteOp op6(request, 5);  // shard2, id: 0, BadValue

    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNErrors(2);
    reply.setCursor(BulkWriteCommandResponseCursor(
        0,
        {
            BulkWriteReplyItem{0, Status(ErrorCodes::BadValue, "")},
            BulkWriteReplyItem{1, Status(CannotImplicitlyCreateCollectionInfo(nss1), "")},
            BulkWriteReplyItem{2, Status::OK()},
        },
        nss1));
    RemoteCommandResponse rcr1(host2, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    auto reply2 = makeReply();
    reply2.setNInserted(1);
    reply2.setNErrors(2);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0,
                                       {
                                           BulkWriteReplyItem{0, Status(ErrorCodes::BadValue, "")},
                                           BulkWriteReplyItem{1, Status::OK()},
                                           BulkWriteReplyItem{2, Status(ErrorCodes::BadValue, "")},
                                       },
                                       nss1));
    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);
    auto result = processor.onWriteBatchResponse({
        {shard2Name,
         Response{
             rcr2,
             {op5, op3, op4},
         }},
        {shard1Name,
         Response{
             rcr1,
             {op6, op1, op2},
         }},
    });
    // Recoverable error.
    ASSERT_TRUE(result.unrecoverableError);
    // Assert all the errors were returned for retry.
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.opsToRetry[0].getId(), 0);
    // Assert nss2 was flagged for creation.
    ASSERT_EQ(result.collsToCreate.size(), 1);
    ASSERT(result.collsToCreate.contains(nss1));

    auto clientReply = processor.generateClientResponse<BulkWriteCommandReply>();
    ASSERT_EQ(clientReply.getNErrors(), 3);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    ASSERT_EQ(clientReply.getNModified(), 0);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 5);
    // Id 0 is not present because the expectation is that it will get retried and filled in, in the
    // next round.
    ASSERT_EQ(batch[0].getIdx(), 1);
    ASSERT_EQ(batch[1].getIdx(), 2);
    ASSERT_EQ(batch[2].getIdx(), 3);
    ASSERT_EQ(batch[3].getIdx(), 4);
    ASSERT_EQ(batch[4].getIdx(), 5);
    ASSERT_EQ(batch[0].getStatus().code(), ErrorCodes::OK);
    ASSERT_EQ(batch[1].getStatus().code(), ErrorCodes::OK);
    ASSERT_EQ(batch[2].getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(batch[3].getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(batch[4].getStatus().code(), ErrorCodes::BadValue);
}

TEST_F(WriteBatchResponseProcessorTest, ProcessesSingleWriteConcernError) {
    // shard1: {code: ok, firstBatch: [{code: ok, originalId: 0, id: 0}, writeConcernError: {}]}
    // shard2: {code: ok, firstBatch: [{code: ok, originalId: 1, id: 0}, writeConcernError: {}]}
    auto request = BulkWriteCommandRequest(
        {
            BulkWriteInsertOp(0, BSON("_id" << 1)),
            BulkWriteInsertOp(1, BSON("_id" << 2)),
        },
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);  // shard1, id: 0, OK
    WriteOp op2(request, 1);  // shard2, id: 2, OK

    auto reply1 = makeReply();
    reply1.setNInserted(1);
    reply1.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    const auto reply1WcErrorMsg = "WriteConcernTimeout";
    reply1.setWriteConcernError(
        BulkWriteWriteConcernError(ErrorCodes::WriteConcernTimeout, reply1WcErrorMsg));
    RemoteCommandResponse rcr1(host2, setTopLevelOK(reply1.toBSON()), Microseconds{0}, false);

    auto reply2 = makeReply();
    reply2.setNInserted(1);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);
    auto result = processor.onWriteBatchResponse({
        {shard1Name,
         Response{
             rcr1,
             {op2},
         }},
        {shard2Name,
         Response{
             rcr2,
             {op1},
         }},
    });
    ASSERT_FALSE(result.unrecoverableError);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponse<BulkWriteCommandReply>();
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 2);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[0].getStatus().code(), ErrorCodes::OK);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[1].getStatus().code(), ErrorCodes::OK);

    // Check write concern errors
    auto wcError = clientReply.getWriteConcernError();
    ASSERT_TRUE(wcError.has_value());
    ASSERT_EQ(wcError->getCode(), ErrorCodes::WriteConcernTimeout);
    ASSERT_TRUE(wcError->getErrmsg().find(reply1WcErrorMsg) != std::string::npos);
}

TEST_F(WriteBatchResponseProcessorTest, ProcessesMultipleWriteConcernErrors) {
    // shard1: {code: ok, firstBatch: [{code: ok, originalId: 0, id: 0}, writeConcernError: {}]}
    // shard2: {code: ok, firstBatch: [{code: ok, originalId: 1, id: 0}, writeConcernError: {}]}
    auto request = BulkWriteCommandRequest(
        {
            BulkWriteInsertOp(0, BSON("_id" << 1)),
            BulkWriteInsertOp(1, BSON("_id" << 2)),
        },
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);  // shard1, id: 0, OK
    WriteOp op2(request, 1);  // shard2, id: 1, OK

    auto reply1 = makeReply();
    reply1.setNInserted(1);
    reply1.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    const auto reply1WcErrorMsg = "WriteConcernTimeout";
    reply1.setWriteConcernError(
        BulkWriteWriteConcernError(ErrorCodes::WriteConcernTimeout, "WriteConcernTimeout"));
    RemoteCommandResponse rcr1(host2, setTopLevelOK(reply1.toBSON()), Microseconds{0}, false);

    auto reply2 = makeReply();
    reply2.setNInserted(1);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    const auto reply2WcErrorMsg = "NotWritablePrimary";
    reply2.setWriteConcernError(
        BulkWriteWriteConcernError(ErrorCodes::NotWritablePrimary, "NotWritablePrimary"));
    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteOpContext ctx(request);
    WriteBatchResponseProcessor processor(ctx);
    auto result = processor.onWriteBatchResponse({
        {shard1Name,
         Response{
             rcr1,
             {op2},
         }},
        {shard2Name,
         Response{
             rcr2,
             {op1},
         }},
    });
    ASSERT_FALSE(result.unrecoverableError);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponse<BulkWriteCommandReply>();
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 2);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[0].getStatus().code(), ErrorCodes::OK);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[1].getStatus().code(), ErrorCodes::OK);

    // Check merged write concern errors
    auto wcError = clientReply.getWriteConcernError();
    ASSERT_TRUE(wcError.has_value());
    ASSERT_EQ(wcError->getCode(), ErrorCodes::WriteConcernTimeout);
    ASSERT_TRUE(wcError->getErrmsg().find(reply1WcErrorMsg) != std::string::npos);
    ASSERT_TRUE(wcError->getErrmsg().find(reply2WcErrorMsg) != std::string::npos);
}
}  // namespace
}  // namespace mongo::unified_write_executor
