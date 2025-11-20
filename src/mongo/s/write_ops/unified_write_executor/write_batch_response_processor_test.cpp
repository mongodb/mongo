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

#include "mongo/base/status.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>

namespace mongo::unified_write_executor {
namespace {

// TODO: SERVER-108526 remove undeed creation of operation context if it's added to
// 'WriteCommandContext'.
class WriteBatchResponseProcessorTest : public ServiceContextTest {
public:
    using RemoteCommandResponse = executor::RemoteCommandResponse;
    using Response = ShardResponse;

    WriteBatchResponseProcessorTest() {
        opCtxHolder = makeOperationContext();
        opCtx = opCtxHolder.get();
    }

    ServiceContext::UniqueOperationContext opCtxHolder;
    OperationContext* opCtx;

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
    const ShardVersion newShardVersion = ShardVersionFactory::make(ChunkVersion(gen, {100, 300}));

    MockRoutingContext routingCtx;

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

    const bool inTransaction = false;
    const bool errorsOnly = true;
    request.setErrorsOnly(errorsOnly);

    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNMatched(3);
    reply.setNModified(3);
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {}, inTransaction, errorsOnly)},
             {shard2Name,
              ShardResponse::make(
                  rcr2, {WriteOp(request, 0), WriteOp(request, 1)}, inTransaction, errorsOnly)}}});

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    ASSERT_EQ(clientReply.getNMatched(), 6);
    ASSERT_EQ(clientReply.getNModified(), 6);

    // Generating a 'BatchedCommandResponse' should output the same statistics, save for 'n', which
    // is the combination of 'nInserted' and 'nMatched', and 'nModified', which is only set on
    // updates.
    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 8);
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
}

TEST_F(WriteBatchResponseProcessorTest, OKRepliesWithUpdateCommand) {
    auto updateRequest = write_ops::UpdateCommandRequest(
        nss1,
        std::vector<write_ops::UpdateOpEntry>{
            write_ops::UpdateOpEntry(BSON("_id" << 0),
                                     write_ops::UpdateModification(BSON("a" << 0))),
            write_ops::UpdateOpEntry(BSON("_id" << 1),
                                     write_ops::UpdateModification(BSON("a" << 1)))});
    auto request = BatchedCommandRequest(updateRequest);

    const bool inTransaction = false;
    const bool errorsOnly = true;

    auto reply = makeReply();
    reply.setNMatched(1);
    reply.setNModified(1);
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {}, inTransaction, errorsOnly)},
             {shard2Name,
              ShardResponse::make(
                  rcr2, {WriteOp(request, 0), WriteOp(request, 1)}, inTransaction, errorsOnly)}}});

    // Generating a 'BatchedCommandResponse' should output the same statistics, save for 'n', which
    // is the combination of 'nInserted' and 'nMatched', and 'nModified', which is only set on
    // updates.
    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 2);
    ASSERT_EQ(*batchedCommandReply.getNModifiedOpt(), 2);
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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)})}}});
    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
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
    request.setOrdered(false);

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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})},
                                  {shard2Name, ShardResponse::make(rcr2, {op2, op3})},
                                  {shard3Name, ShardResponse::make(rcr3, {op4})}}});

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op2})}}});
    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
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
    auto clientReply = tempProcessor.generateClientResponseForBulkWriteCommand(opCtx);
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

    result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr2, {op2})}}});
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
    ASSERT(result.opsToRetry.empty());
    ASSERT(result.collsToCreate.empty());
    clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);

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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op2, op3})}}});
    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
    // Assert all ops were returned for retry even though there was only one item in the reply.
    ASSERT_EQ(result.opsToRetry.size(), 3);
    ASSERT_EQ(result.opsToRetry[0].getId(), 0);
    ASSERT_EQ(result.opsToRetry[1].getId(), 1);
    ASSERT_EQ(result.opsToRetry[2].getId(), 2);
    // Assert nss1 was flagged for creation.

    ASSERT_EQ(result.collsToCreate.size(), 1);
    ASSERT(result.collsToCreate.contains(nss1));

    // Assert the generated response is as expected.
    auto response = processor.generateClientResponseForBulkWriteCommand(opCtx);
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
    request.setOrdered(false);

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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op2, op3})},
                                  {shard2Name, ShardResponse::make(rcr2, {op4, op5, op6})}}});
    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
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
    request.setOrdered(false);

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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard2Name, ShardResponse::make(rcr2, {op5, op3, op4})},
                                  {shard1Name, ShardResponse::make(rcr1, {op6, op1, op2})}}});
    // Errors should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 3);

    // Assert all the errors were returned for retry.
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.opsToRetry[0].getId(), 0);
    // Assert nss2 was flagged for creation.
    ASSERT_EQ(result.collsToCreate.size(), 1);
    ASSERT(result.collsToCreate.contains(nss1));

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
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

TEST_F(WriteBatchResponseProcessorTest, RetryStalenessErrors) {
    // shard1: {code: ok, firstBatch: [{code: StaleShardVersion}, {code: StaleDbVersion}]}
    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(1, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    request.setOrdered(false);

    WriteOp op1(request, 0);
    WriteOp op2(request, 1);

    Status staleCollStatus(
        StaleConfigInfo(nss1, *shard1Endpoint.shardVersion, newShardVersion, shard1Name), "");
    const ShardEndpoint shard1EndpointUnsharded =
        ShardEndpoint(shard1Name,
                      ShardVersionFactory::make(ChunkVersion::UNTRACKED()),
                      DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseVersion newDbVersion(UUID::gen(), Timestamp(1, 100));
    Status staleDbStatus(StaleDbRoutingVersion(
                             nss2.dbName(), *shard1EndpointUnsharded.databaseVersion, newDbVersion),
                         "");

    auto reply = makeReply();
    reply.setNErrors(1);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       BulkWriteReplyItem{0, staleCollStatus},
                                                       BulkWriteReplyItem{1, staleDbStatus},
                                                   },
                                                   nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op2})}}});

    ASSERT_EQ(routingCtx.errors.size(), 2);
    ASSERT_EQ(routingCtx.errors[0].code(), ErrorCodes::StaleConfig);
    ASSERT_EQ(routingCtx.errors[0].extraInfo<StaleConfigInfo>()->getNss(), nss1);
    ASSERT_EQ(routingCtx.errors[1].code(), ErrorCodes::StaleDbVersion);
    ASSERT_EQ(routingCtx.errors[1].extraInfo<StaleDbRoutingVersion>()->getDb(), nss2.dbName());

    // Assert all the op was returned for retry.
    ASSERT_EQ(result.opsToRetry.size(), 2);
    ASSERT_EQ(result.opsToRetry[0].getId(), 0);
    ASSERT_EQ(result.opsToRetry[0].getNss(), nss1);
    ASSERT_EQ(result.opsToRetry[1].getId(), 1);
    ASSERT_EQ(result.opsToRetry[1].getNss(), nss2);
    ASSERT(result.collsToCreate.empty());

    // Assert errors was not incremented since we can retry Staleness errors.
    auto response = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(response.getNErrors(), 0);
    ASSERT_EQ(response.getNInserted(), 0);
}

TEST_F(WriteBatchResponseProcessorTest, MixedStalenessErrorsAndOk) {
    // shard1: {code: ok, firstBatch: [{code: StaleShardVersion}, {code: ok}]}
    // shard2: {code: ok, firstBatch: [{code: StaleShardVersion}, {code: ok}]}
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(0, BSON("_id" << -1)),
                                            BulkWriteInsertOp(1, BSON("_id" << 1)),
                                            BulkWriteInsertOp(1, BSON("_id" << -1))},
                                           {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    request.setOrdered(false);

    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);
    WriteOp op4(request, 3);
    Status staleCollStatus1(
        StaleConfigInfo(nss1, *shard1Endpoint.shardVersion, newShardVersion, shard1Name), "");
    Status staleCollStatus2(
        StaleConfigInfo(nss1, *shard2Endpoint.shardVersion, newShardVersion, shard2Name), "");

    auto reply = makeReply();
    reply.setNErrors(1);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       BulkWriteReplyItem{0, staleCollStatus1},
                                                       BulkWriteReplyItem{1, Status::OK()},
                                                   },
                                                   nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    auto reply2 = makeReply();
    reply2.setNErrors(1);
    reply2.setCursor(BulkWriteCommandResponseCursor(0,
                                                    {
                                                        BulkWriteReplyItem{0, staleCollStatus2},
                                                        BulkWriteReplyItem{1, Status::OK()},
                                                    },
                                                    nss2));
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op3})},
                                  {shard2Name, ShardResponse::make(rcr2, {op2, op4})}}});

    ASSERT_EQ(routingCtx.errors.size(), 2);
    ASSERT_EQ(routingCtx.errors[0].code(), ErrorCodes::StaleConfig);
    ASSERT_EQ(routingCtx.errors[0].extraInfo<StaleConfigInfo>()->getNss(), nss1);
    ASSERT_EQ(routingCtx.errors[1].code(), ErrorCodes::StaleConfig);
    ASSERT_EQ(routingCtx.errors[1].extraInfo<StaleConfigInfo>()->getNss(), nss1);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    // Assert failed ops were returned for retry.
    ASSERT_EQ(result.opsToRetry.size(), 2);
    ASSERT_EQ(result.opsToRetry[0].getId(), op1.getId());
    ASSERT_EQ(result.opsToRetry[0].getNss(), nss1);
    ASSERT_EQ(result.opsToRetry[1].getId(), op2.getId());
    ASSERT_EQ(result.opsToRetry[1].getNss(), nss1);
}

TEST_F(WriteBatchResponseProcessorTest, RetryShardsCannotRefreshDueToLocksHeldError) {
    // shard1: {code: ok, firstBatch: [{code: ShardCannotRefreshDueToLocksHeld}, {code: ok}]}
    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(1, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    request.setOrdered(false);

    WriteOp op1(request, 0);
    WriteOp op2(request, 1);

    auto shardCannotRefreshError = Status{ShardCannotRefreshDueToLocksHeldInfo(nss1),
                                          "Mock error: Catalog cache busy in refresh"};

    auto reply = makeReply();
    reply.setNErrors(1);
    reply.setNInserted(1);
    reply.setCursor(
        BulkWriteCommandResponseCursor(0,
                                       {
                                           BulkWriteReplyItem{0, shardCannotRefreshError},
                                           BulkWriteReplyItem{1, Status::OK()},
                                       },
                                       nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op2})}}});

    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);

    // Assert the only op1 was returned for retry.
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.opsToRetry[0].getId(), 0);
    ASSERT_EQ(result.opsToRetry[0].getNss(), nss1);
    ASSERT(result.collsToCreate.empty());

    // Assert errors was not incremented since we can retry ShardsCannotRefresh errors.
    auto response = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(response.getNErrors(), 0);
    ASSERT_EQ(response.getNInserted(), 1);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_FALSE(batchedCommandReply.isErrDetailsSet());
    ASSERT_EQ(batchedCommandReply.getN(), 1);
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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op2})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1})}}});
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
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

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(opCtx,
                                                 routingCtx,
                                                 SimpleWriteBatchResponse{{
                                                     {shard1Name, ShardResponse::make(rcr1, {op2})},
                                                     {shard2Name, ShardResponse::make(rcr2, {op1})},
                                                 }});
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
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

TEST_F(WriteBatchResponseProcessorTest, ProcessesExceededMemoryLimitError) {
    RAIIServerParameterControllerForTest maxRepliesSizeController("bulkWriteMaxRepliesSize", 20);

    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(0, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1)});
    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNMatched(0);
    reply.setNModified(0);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       BulkWriteReplyItem{0, Status::OK()},
                                                   },
                                                   nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef ctx(request);

    {
        Stats stats;
        WriteBatchResponseProcessor processor(ctx, stats);

        // It is not possible to exceeded the size limit if we have yet to process the response.
        ASSERT_FALSE(processor.checkBulkWriteReplyMaxSize(opCtx));
        ASSERT_EQ(processor.getNumErrorsRecorded(), 0);

        // If we are able to batch all writes together, we should not exceed the memory limit. This
        // is because any memory limit is recorded as an error for the first incomplete WriteOp,
        // which we do not have if we have batched all writes together.
        auto result = processor.onWriteBatchResponse(
            opCtx,
            routingCtx,
            SimpleWriteBatchResponse{
                {{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)})},
                 {shard2Name, ShardResponse::make(rcr1, {WriteOp(request, 1)})}}});

        ASSERT_FALSE(processor.checkBulkWriteReplyMaxSize(opCtx));
        auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
        ASSERT_EQ(clientReply.getNErrors(), 0);

        // Should have an OK response.
        auto batch = clientReply.getCursor().getFirstBatch();
        ASSERT_EQ(batch.size(), 2);
        ASSERT_EQ(batch[0].getIdx(), 0);
        ASSERT_EQ(batch[1].getIdx(), 1);
        ASSERT_EQ(batch[0].getStatus().code(), Status::OK());
        ASSERT_EQ(batch[1].getStatus().code(), Status::OK());
    }

    // This time, we only get a response for a single WriteOp. We should detect that the memory
    // limit error has been exceeded and mark the incomplete WriteOp as having failed.
    {
        Stats stats;
        WriteBatchResponseProcessor processor(ctx, stats);

        auto result = processor.onWriteBatchResponse(
            opCtx,
            routingCtx,
            SimpleWriteBatchResponse{
                {{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)})}}});

        ASSERT_TRUE(processor.checkBulkWriteReplyMaxSize(opCtx));
        ASSERT_EQ(processor.getNumErrorsRecorded(), 1);
        auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
        ASSERT_EQ(clientReply.getNErrors(), 1);

        // Should have an OK response even if the last error is terminal.
        auto batch = clientReply.getCursor().getFirstBatch();
        ASSERT_EQ(batch.size(), 2);
        ASSERT_EQ(batch[0].getIdx(), 0);
        ASSERT_EQ(batch[1].getIdx(), 1);
        ASSERT_EQ(batch[0].getStatus().code(), Status::OK());
        ASSERT_EQ(batch[1].getStatus().code(), ErrorCodes::ExceededMemoryLimit);
    }
}

TEST_F(WriteBatchResponseProcessorTest, IncrementApproxSizeOnceForRetry) {
    // Set the limit to just above the reply size for a single op.
    RAIIServerParameterControllerForTest maxRepliesSizeController("bulkWriteMaxRepliesSize", 35);
    //  shard1: {code: ok, firstBatch: [{code: ok}, {code: CannotImplicitlyCreateCollection}]}
    // Note that we add a third op to the request such that we can detect that the memory limit has
    // been exceeded after our retry.
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(1, BSON("_id" << 2)),
                                            BulkWriteInsertOp(1, BSON("_id" << 3))},
                                           {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);
    BulkWriteReplyItem replyOk = BulkWriteReplyItem{0, Status::OK()};
    BulkWriteReplyItem replyRetry =
        BulkWriteReplyItem{1, Status(CannotImplicitlyCreateCollectionInfo(nss2), "")};

    auto reply = makeReply();
    reply.setNErrors(1);
    reply.setNInserted(1);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       replyOk,
                                                       replyRetry,
                                                   },
                                                   nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef ctx(request);
    Stats stats;
    WriteBatchResponseProcessor processor(ctx, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op2})}}});
    // No unrecoverable error.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);

    // One incomplete returned (op2).
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.opsToRetry[0].getNss(), nss2);
    ASSERT_EQ(result.opsToRetry[0].getId(), 1);

    // We should not have exceeded the max size because we've only incremented the non-retry item.
    ASSERT_FALSE(processor.checkBulkWriteReplyMaxSize(opCtx));

    // Assert nss2 was flagged for creation.
    ASSERT_EQ(result.collsToCreate.size(), 1);
    ASSERT(result.collsToCreate.contains(nss2));

    // Simulate a successful retry.
    reply = makeReply();
    reply.setNInserted(1);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       replyOk,
                                                   },
                                                   nss2));

    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr2, {op2})}}});

    // On successful retry, we should have exceeded the memory limit.
    ASSERT_TRUE(processor.checkBulkWriteReplyMaxSize(opCtx));
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);
    ASSERT(result.opsToRetry.empty());
    ASSERT(result.collsToCreate.empty());
}

TEST_F(WriteBatchResponseProcessorTest, ProcessesNoRetryResponseOk) {
    auto updateRequest = write_ops::UpdateCommandRequest(
        nss1,
        std::vector<write_ops::UpdateOpEntry>{write_ops::UpdateOpEntry(
            BSON("_id" << 0), write_ops::UpdateModification(BSON("a" << 0)))});
    auto request = BatchedCommandRequest(updateRequest);

    const bool inTransaction = false;
    const bool errorsOnly = true;

    auto reply = makeReply();
    reply.setNMatched(1);
    reply.setNModified(1);
    reply.setCursor(BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        NoRetryWriteBatchResponse::make(StatusWith<BSONObj>(reply.toBSON()),
                                        /*wce*/ boost::none,
                                        WriteOp(request, 0),
                                        inTransaction,
                                        errorsOnly));
    ASSERT_TRUE(result.opsToRetry.empty());
    ASSERT_TRUE(result.collsToCreate.empty());

    ASSERT_EQ(processor.getNumOkItemsProcessed(), 1);
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 1);
    ASSERT_EQ(*batchedCommandReply.getNModifiedOpt(), 1);
    ASSERT_FALSE(batchedCommandReply.isErrDetailsSet());
}

TEST_F(WriteBatchResponseProcessorTest, ProcessesNoRetryResponseError) {
    auto updateRequest = write_ops::UpdateCommandRequest(
        nss1,
        std::vector<write_ops::UpdateOpEntry>{write_ops::UpdateOpEntry(
            BSON("_id" << 0), write_ops::UpdateModification(BSON("a" << 0)))});
    auto request = BatchedCommandRequest(updateRequest);

    const bool inTransaction = false;
    const bool errorsOnly = true;

    auto reply = makeReply();
    reply.setNMatched(1);
    reply.setNModified(0);
    reply.setCursor(BulkWriteCommandResponseCursor(
        0, {BulkWriteReplyItem{0, Status(ErrorCodes::BadValue, "Wrong argument")}}, nss1));

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        NoRetryWriteBatchResponse::make(StatusWith<BSONObj>(reply.toBSON()),
                                        /*wce*/ boost::none,
                                        WriteOp(request, 0),
                                        inTransaction,
                                        errorsOnly));
    ASSERT_TRUE(result.opsToRetry.empty());
    ASSERT_TRUE(result.collsToCreate.empty());

    ASSERT_EQ(processor.getNumOkItemsProcessed(), 0);
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 1);
    ASSERT_EQ(*batchedCommandReply.getNModifiedOpt(), 0);
    ASSERT_EQ(batchedCommandReply.getErrDetails().size(), 1);
    ASSERT_EQ(batchedCommandReply.getErrDetails()[0].getStatus().code(), ErrorCodes::BadValue);
}

TEST_F(WriteBatchResponseProcessorTest, NonVerboseMode) {
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
    RemoteCommandResponse rcr1(host2, setTopLevelOK(reply1.toBSON()), Microseconds{0}, false);

    auto reply2 = makeReply();
    reply2.setNInserted(1);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats, true /*isNonVerbose*/);
    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op2})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1})}}});

    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 0);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_FALSE(batchedCommandReply.getNOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.isErrDetailsSet());
}

TEST_F(WriteBatchResponseProcessorTest, NonVerboseModeWithErrors) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1))},
                                           {NamespaceInfoEntry(nss1)});
    WriteOp op1(request, 0);

    RemoteCommandResponse rcr1(
        host1,
        ErrorReply(false, ErrorCodes::BadValue, "Bad Value", "simulating error").toBSON(),
        Microseconds{0},
        false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats, true /*isNonVerbose*/);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})}}});

    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNInserted(), 0);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 0);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_FALSE(batchedCommandReply.getNOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.isErrDetailsSet());
}

TEST_F(WriteBatchResponseProcessorTest, NonVerboseModeWithMixedErrorsAndOk) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(0, BSON("_id" << 2)),
                                            BulkWriteInsertOp(0, BSON("_id" << 3))},
                                           {NamespaceInfoEntry(nss1)});
    request.setOrdered(false);

    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);

    auto reply1 = makeReply();
    reply1.setNInserted(1);
    reply1.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply1.toBSON()), Microseconds{0}, false);

    auto reply2 = BulkWriteCommandReply(
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
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats, true /*isNonVerbose*/);

    processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})},
                                  {shard2Name, ShardResponse::make(rcr2, {op2, op3})}}});

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 0);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_FALSE(batchedCommandReply.getNOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.isErrDetailsSet());
}

TEST_F(WriteBatchResponseProcessorTest, MultiWritesOKReplies) {
    auto update1 = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2)));
    auto update2 = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -3 << "$lt" << 3)), BSON("$set" << BSON("z" << 3)));
    auto update3 = BulkWriteUpdateOp(
        0, BSON("y" << BSON("$gte" << -3 << "$lt" << 3)), BSON("$set" << BSON("z" << 1)));
    update1.setMulti(true);
    update2.setMulti(true);
    update3.setMulti(true);
    auto request = BulkWriteCommandRequest({update1, update2, update3}, {NamespaceInfoEntry(nss1)});

    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);

    ShardId shard3Name = ShardId("shard3");
    ShardEndpoint shard3Endpoint = ShardEndpoint(
        shard3Name, ShardVersionFactory::make(ChunkVersion(gen, {100, 200})), boost::none);
    HostAndPort host3 = HostAndPort("host3", 0);

    // Response for shard1.
    auto reply = makeReply();
    reply.setNModified(3);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {BulkWriteReplyItem{0, Status::OK()},
                                                    BulkWriteReplyItem{1, Status::OK()},
                                                    BulkWriteReplyItem{2, Status::OK()}},
                                                   nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    // Response for shard2.
    auto reply2 = makeReply();
    reply2.setNModified(1);
    reply2.setCursor(BulkWriteCommandResponseCursor(0,
                                                    {BulkWriteReplyItem{0, Status::OK()},
                                                     BulkWriteReplyItem{1, Status::OK()},
                                                     BulkWriteReplyItem{2, Status::OK()}},
                                                    nss1));
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    // Response for shard3.
    auto reply3 = makeReply();
    reply3.setNModified(6);
    reply3.setCursor(BulkWriteCommandResponseCursor(0,
                                                    {BulkWriteReplyItem{0, Status::OK()},
                                                     BulkWriteReplyItem{1, Status::OK()},
                                                     BulkWriteReplyItem{2, Status::OK()}},
                                                    nss1));
    RemoteCommandResponse rcr3(host1, setTopLevelOK(reply3.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op2, op3})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1, op2, op3})},
                                  {shard3Name, ShardResponse::make(rcr3, {op1, op2, op3})}}});

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNModified(), 10);
    ASSERT_EQ(clientReply.getNInserted(), 0);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 3);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[2].getIdx(), 2);
    ASSERT_EQ(batch[0].getStatus(), Status::OK());
    ASSERT_EQ(batch[1].getStatus(), Status::OK());
    ASSERT_EQ(batch[2].getStatus(), Status::OK());
}

TEST_F(WriteBatchResponseProcessorTest, MixedMultiAndNonMultiWritesOKReplies) {
    auto update1 = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2)));
    auto update2 = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -3 << "$lt" << 3)), BSON("$set" << BSON("z" << 3)));
    auto insert1 = BulkWriteInsertOp(0, BSON("_id" << 3));
    update1.setMulti(true);
    update2.setMulti(true);
    auto request = BulkWriteCommandRequest({update1, insert1, update2}, {NamespaceInfoEntry(nss1)});

    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);

    ShardId shard3Name = ShardId("shard3");
    ShardEndpoint shard3Endpoint = ShardEndpoint(
        shard3Name, ShardVersionFactory::make(ChunkVersion(gen, {100, 200})), boost::none);
    HostAndPort host3 = HostAndPort("host3", 0);

    // Response for shard1.
    auto reply = makeReply();
    reply.setNModified(3);
    reply.setCursor(BulkWriteCommandResponseCursor(
        0, {BulkWriteReplyItem{0, Status::OK()}, BulkWriteReplyItem{1, Status::OK()}}, nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    // Response for shard2.
    auto reply2 = makeReply();
    reply2.setNModified(2);
    reply2.setNInserted(1);
    reply2.setCursor(BulkWriteCommandResponseCursor(0,
                                                    {BulkWriteReplyItem{0, Status::OK()},
                                                     BulkWriteReplyItem{1, Status::OK()},
                                                     BulkWriteReplyItem{2, Status::OK()}},
                                                    nss1));
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    // Response for shard3.
    auto reply3 = makeReply();
    reply3.setNModified(6);
    reply3.setCursor(BulkWriteCommandResponseCursor(
        0, {BulkWriteReplyItem{0, Status::OK()}, BulkWriteReplyItem{1, Status::OK()}}, nss1));
    RemoteCommandResponse rcr3(host1, setTopLevelOK(reply3.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1, op3})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1, op2, op3})},
                                  {shard3Name, ShardResponse::make(rcr3, {op1, op3})}}});

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNModified(), 11);
    ASSERT_EQ(clientReply.getNInserted(), 1);
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 3);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[2].getIdx(), 2);
    ASSERT_EQ(batch[0].getStatus(), Status::OK());
    ASSERT_EQ(batch[1].getStatus(), Status::OK());
    ASSERT_EQ(batch[2].getStatus(), Status::OK());
}

TEST_F(WriteBatchResponseProcessorTest, MultiWriteMixedOKAndRetryableErrorThenOK) {
    auto update = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2)));
    update.setMulti(true);

    auto request = BulkWriteCommandRequest({update}, {NamespaceInfoEntry(nss1)});

    WriteOp op1(request, 0);

    // Response for shard1.
    auto reply = makeReply();
    const ShardEndpoint shard1EndpointUnsharded =
        ShardEndpoint(shard1Name,
                      ShardVersionFactory::make(ChunkVersion::UNTRACKED()),
                      DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseVersion newDbVersion(UUID::gen(), Timestamp(1, 100));
    Status staleDbStatus(StaleDbRoutingVersion(
                             nss2.dbName(), *shard1EndpointUnsharded.databaseVersion, newDbVersion),
                         "");
    reply.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, staleDbStatus}}, nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    // Response for shard2.
    auto reply2 = makeReply();
    reply2.setNModified(1);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1})}}});

    ASSERT_EQ(result.successfulShardSet.size(), 1);
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    // OK response from shard 1 on retry.
    RemoteCommandResponse rcr3(host1, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    auto nextResult = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr3, {op1})}}});

    ASSERT_EQ(nextResult.successfulShardSet.size(), 1);
    ASSERT_EQ(nextResult.opsToRetry.size(), 0);
    ASSERT_EQ(nextResult.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNModified(), 2);

    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[0].getStatus(), Status::OK());
}

TEST_F(WriteBatchResponseProcessorTest, MultiWriteMixedOKAndRetryableErrorThenNonRetryable) {
    auto update = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2)));
    update.setMulti(true);

    auto request = BulkWriteCommandRequest({update}, {NamespaceInfoEntry(nss1)});

    WriteOp op1(request, 0);

    // Response for shard1.
    auto reply = makeReply();
    const ShardEndpoint shard1EndpointUnsharded =
        ShardEndpoint(shard1Name,
                      ShardVersionFactory::make(ChunkVersion::UNTRACKED()),
                      DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseVersion newDbVersion(UUID::gen(), Timestamp(1, 100));
    Status staleDbStatus(StaleDbRoutingVersion(
                             nss2.dbName(), *shard1EndpointUnsharded.databaseVersion, newDbVersion),
                         "");
    reply.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, staleDbStatus}}, nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    // Response for shard2.
    auto reply2 = makeReply();
    reply2.setNModified(1);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1})}}});


    ASSERT_EQ(result.successfulShardSet.size(), 1);
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    const auto errorCode = ErrorCodes::BadValue;
    const auto errorMsg = "CustomError";
    RemoteCommandResponse rcr3(
        host1,
        [&errorCode, &errorMsg] {
            auto error = ErrorReply(0, errorCode, errorMsg, errorMsg);
            return error.toBSON();
        }(),
        Microseconds{0},
        false);

    auto nextResult = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr3, {op1})}}});

    ASSERT_EQ(nextResult.successfulShardSet.size(), 0);
    ASSERT_EQ(nextResult.opsToRetry.size(), 0);
    ASSERT_EQ(nextResult.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNModified(), 1);

    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[0].getStatus(), Status(errorCode, errorMsg));
}

TEST_F(WriteBatchResponseProcessorTest, MultiWriteMixedOKAndNonRetryableError) {
    auto update = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2)));
    update.setMulti(true);

    auto request = BulkWriteCommandRequest({update}, {NamespaceInfoEntry(nss1)});

    WriteOp op1(request, 0);

    const auto errorCode = ErrorCodes::BadValue;
    const auto errorMsg = "CustomError";
    RemoteCommandResponse rcr1(
        host1,
        [&errorCode, &errorMsg] {
            auto error = ErrorReply(0, errorCode, errorMsg, errorMsg);
            return error.toBSON();
        }(),
        Microseconds{0},
        false);

    // Response for shard2.
    auto reply2 = makeReply();
    reply2.setNModified(1);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1})}}});


    ASSERT_EQ(result.successfulShardSet.size(), 1);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNModified(), 1);

    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[0].getStatus(), Status(errorCode, errorMsg));
}

TEST_F(WriteBatchResponseProcessorTest, MultiWriteNonRetryableErrors) {
    auto update = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2)));
    update.setMulti(true);

    auto request = BulkWriteCommandRequest({update}, {NamespaceInfoEntry(nss1)});

    WriteOp op1(request, 0);

    const auto errorCode = ErrorCodes::BadValue;
    const auto errorMsg = "Custom error";
    RemoteCommandResponse rcr1(
        host1,
        [&errorCode, &errorMsg] {
            auto error = ErrorReply(0, errorCode, "Custom error", errorMsg);
            return error.toBSON();
        }(),
        Microseconds{0},
        false);

    const auto errorCode2 = ErrorCodes::InvalidOptions;
    const auto errorMsg2 = "invalid options error message";

    auto reply = makeReply();
    reply.setCursor(
        BulkWriteCommandResponseCursor(0,
                                       {
                                           BulkWriteReplyItem{0, Status(errorCode2, errorMsg2)},
                                       },
                                       nss1));
    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1})}}});


    ASSERT_EQ(result.successfulShardSet.size(), 0);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNModified(), 0);

    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 0);

    BSONArrayBuilder errArr;
    auto w1 = write_ops::WriteError(0, Status(ErrorCodes::BadValue, errorMsg));
    auto w2 = write_ops::WriteError(0, Status(ErrorCodes::InvalidOptions, errorMsg2));
    errArr.append(w1.serialize());
    errArr.append(w2.serialize());

    std::stringstream msg("multiple errors for op : ");
    msg << errorMsg << " :: and :: " << errorMsg2;

    ASSERT_EQ(batch[0].getStatus(), Status(MultipleErrorsOccurredInfo(errArr.arr()), msg.str()));
}

TEST_F(WriteBatchResponseProcessorTest, MultiWriteRetryableNonRetryableAndOK) {
    auto update1 = BulkWriteUpdateOp(
        0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2)));
    update1.setMulti(true);
    auto request = BulkWriteCommandRequest({update1}, {NamespaceInfoEntry(nss1)});

    WriteOp op1(request, 0);

    ShardId shard3Name = ShardId("shard3");
    ShardEndpoint shard3Endpoint = ShardEndpoint(
        shard3Name, ShardVersionFactory::make(ChunkVersion(gen, {100, 200})), boost::none);
    HostAndPort host3 = HostAndPort("host3", 0);

    // Response for shard1.
    auto reply = makeReply();
    const ShardEndpoint shard1EndpointUnsharded =
        ShardEndpoint(shard1Name,
                      ShardVersionFactory::make(ChunkVersion::UNTRACKED()),
                      DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseVersion newDbVersion(UUID::gen(), Timestamp(1, 100));
    Status staleDbStatus(StaleDbRoutingVersion(
                             nss2.dbName(), *shard1EndpointUnsharded.databaseVersion, newDbVersion),
                         "");
    reply.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, staleDbStatus}}, nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    // Response for shard2.
    const auto errorCode = ErrorCodes::InvalidOptions;
    const auto errorMsg = "invalid options error message";
    RemoteCommandResponse rcr2(
        host1,
        [&errorCode, &errorMsg] {
            auto error = ErrorReply(0, errorCode, "InvalidOptions", errorMsg);
            return error.toBSON();
        }(),
        Microseconds{0},
        false);

    // Response for shard3.
    auto reply3 = makeReply();
    reply3.setNModified(6);
    reply3.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr3(host1, setTopLevelOK(reply3.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {op1})},
                                  {shard2Name, ShardResponse::make(rcr2, {op1})},
                                  {shard3Name, ShardResponse::make(rcr3, {op1})}}});


    ASSERT_EQ(result.successfulShardSet.size(), 1);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNModified(), 6);

    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 0);

    ASSERT_EQ(batch[0].getStatus(), Status(errorCode, errorMsg));
}

TEST_F(WriteBatchResponseProcessorTest, ProcessFindAndModifyOKResponse) {
    write_ops::FindAndModifyCommandRequest request(nss1);
    request.setQuery(BSON("a" << 1));
    request.setUpdate(write_ops::UpdateModification(BSON("b" << 1)));
    WriteOp op(request);

    write_ops::FindAndModifyLastError lastError;
    lastError.setNumDocs(1);
    lastError.setUpdatedExisting(true);
    write_ops::FindAndModifyCommandReply reply;
    reply.setLastErrorObject(std::move(lastError));
    reply.setValue(BSON("a" << 1));

    ShardId shard1 = ShardId("shard1");
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx, routingCtx, SimpleWriteBatchResponse{{{shard1, ShardResponse::make(rcr1, {op})}}});
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    ASSERT_EQ(processor.getNumOkItemsProcessed(), 1);
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);

    auto response = processor.generateClientResponseForFindAndModifyCommand();
    ASSERT(response.swReply.isOK());
    auto clientReply = response.swReply.getValue();
    ASSERT_BSONOBJ_EQ(clientReply.toBSON(), reply.toBSON());
}

TEST_F(WriteBatchResponseProcessorTest, ProcessFindAndModifyErrorResponse) {
    write_ops::FindAndModifyCommandRequest request(nss1);
    request.setQuery(BSON("a" << 1));
    request.setUpdate(write_ops::UpdateModification(BSON("b" << 1)));
    WriteOp op(request);

    ErrorReply reply(0 /* ok */,
                     ErrorCodes::BadValue,
                     "Bad Value" /* codeName */,
                     "Wrong argument" /* errmsg */);

    ShardId shard1 = ShardId("shard1");
    RemoteCommandResponse rcr1(host1, reply.toBSON(), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx, routingCtx, SimpleWriteBatchResponse{{{shard1, ShardResponse::make(rcr1, {op})}}});
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    ASSERT_EQ(processor.getNumOkItemsProcessed(), 0);
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);

    auto response = processor.generateClientResponseForFindAndModifyCommand();
    ASSERT(!response.swReply.isOK());
    auto status = response.swReply.getStatus();
    ASSERT_EQ(status.code(), reply.getCode());
    ASSERT_EQ(status.reason(), reply.getErrmsg());
}

TEST_F(WriteBatchResponseProcessorTest, ProcessFindAndModifyRetryResponse) {
    write_ops::FindAndModifyCommandRequest request(nss1);
    request.setQuery(BSON("a" << 1));
    request.setUpdate(write_ops::UpdateModification(BSON("b" << 1)));
    WriteOp op(request);

    ShardId shard1 = ShardId("shard1");
    ErrorReply errorReply(0 /* ok */,
                          ErrorCodes::StaleConfig,
                          "StaleConfig" /* codeName */,
                          "Shard stale error, please retry" /* errmsg */);
    StaleConfigInfo staleInfo(nss1, *shard1Endpoint.shardVersion, boost::none /* wanted */, shard1);
    BSONObjBuilder builder;
    errorReply.serialize(&builder);
    staleInfo.serialize(&builder);
    auto reply = builder.obj();

    RemoteCommandResponse rcr1(host1, reply, Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx, routingCtx, SimpleWriteBatchResponse{{{shard1, ShardResponse::make(rcr1, {op})}}});
    ASSERT_EQ(result.collsToCreate.size(), 0);
    ASSERT_EQ(result.opsToRetry.size(), 1);
    ASSERT_EQ(result.opsToRetry[0].getId(), op.getId());

    ASSERT_EQ(processor.getNumOkItemsProcessed(), 0);
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
}

class WriteBatchResponseProcessorTxnTest : public WriteBatchResponseProcessorTest {
public:
    void setUp() override {
        WriteBatchResponseProcessorTest::setUp();

        // Setup transaction.
        auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
        opCtx->setLogicalSessionId(lsid);
        TxnNumber txnNumber = 0;
        opCtx->setTxnNumber(txnNumber);
    }
};

TEST_F(WriteBatchResponseProcessorTxnTest, OKReplies) {
    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(0, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1)});

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNMatched(0);
    reply.setNModified(0);
    reply.setCursor(BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));

    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)}, inTransaction)},
             {shard2Name, ShardResponse::make(rcr2, {WriteOp(request, 1)}, inTransaction)}}});

    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);

    // Confirm the generated bulk reply and batched command response are both correct.
    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    ASSERT_EQ(clientReply.getNMatched(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 2);
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.isErrDetailsSet());
}

TEST_F(WriteBatchResponseProcessorTxnTest, TransientTransactionErrorInARSThrows) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1))},
                                           {NamespaceInfoEntry(nss1)});
    WriteOp op1(request, 0);

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    // Shard response with a transient transaction error label from the ARS.
    const auto errorCode = ErrorCodes::PreparedTransactionInProgress;
    StatusWith<executor::RemoteCommandResponse> rcr1 =
        StatusWith<executor::RemoteCommandResponse>(Status(errorCode, "CustomError"));

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    ASSERT_THROWS_CODE(processor.onWriteBatchResponse(
                           opCtx,
                           routingCtx,
                           SimpleWriteBatchResponse{
                               {{shard1Name, ShardResponse::make(rcr1, {op1}, inTransaction)}}}),
                       DBException,
                       errorCode);
}

TEST_F(WriteBatchResponseProcessorTxnTest,
       TransientTransactionErrorInARSMultipleShardResponsesThrows) {
    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(1, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    // Shard response with a transient transaction error label from the ARS.
    const auto errorCode = ErrorCodes::PreparedTransactionInProgress;
    StatusWith<executor::RemoteCommandResponse> rcr1 =
        StatusWith<executor::RemoteCommandResponse>(Status(errorCode, "CustomError"));

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNMatched(0);
    reply.setNModified(0);
    reply.setCursor(BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss2));
    RemoteCommandResponse rcr2(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    ASSERT_THROWS_CODE(processor.onWriteBatchResponse(
                           opCtx,
                           routingCtx,
                           SimpleWriteBatchResponse{
                               {{shard1Name, ShardResponse::make(rcr1, {op1}, inTransaction)},
                                {shard2Name, ShardResponse::make(rcr2, {op2}, inTransaction)}}}),
                       DBException,
                       errorCode);
}

TEST_F(WriteBatchResponseProcessorTxnTest, NonTransientTransactionErrorInARSHaltsProcessing) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(0, BSON("_id" << 2)),
                                            BulkWriteInsertOp(0, BSON("_id" << 3))},
                                           {NamespaceInfoEntry(nss1)});

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNMatched(0);
    reply.setNModified(0);
    reply.setCursor(BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    const auto errorCode = ErrorCodes::BadValue;
    const auto errorMsg = "CustomError";
    StatusWith<executor::RemoteCommandResponse> rcr2 =
        StatusWith<executor::RemoteCommandResponse>(Status(errorCode, errorMsg));

    ShardId shard3Name = ShardId("shard3");

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)}, inTransaction)},
             {shard2Name, ShardResponse::make(rcr2, {WriteOp(request, 1)}, inTransaction)},
             {shard3Name, ShardResponse::makeEmpty({WriteOp(request, 2)})}}});

    // An error should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);

    // Confirm the generated bulk reply and batched command response are both correct.
    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNInserted(), 1);
    ASSERT_EQ(clientReply.getNMatched(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);
    ASSERT_EQ(clientReply.getNErrors(), 1);

    // Assert we don't return any results after the error.
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 2);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[0].getStatus(), Status::OK());
    ASSERT_EQ(batch[1].getStatus(), Status(errorCode, errorMsg));

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 1);
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    const auto errors = batchedCommandReply.getErrDetails();
    ASSERT_EQ(errors.size(), 1);
    ASSERT_EQ(errors[0].getIndex(), 1);
    ASSERT_EQ(errors[0].getStatus(), Status(errorCode, errorMsg));
}

TEST_F(WriteBatchResponseProcessorTxnTest, TransientTransactionErrorInShardResponseThrows) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1))},
                                           {NamespaceInfoEntry(nss1)});
    WriteOp op1(request, 0);

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    // Shard response with a transient transaction error label.
    const auto errorCode = ErrorCodes::PreparedTransactionInProgress;
    RemoteCommandResponse rcr1(
        host1,
        [&errorCode] {
            auto error = ErrorReply(0, errorCode, "CustomError", "custom error for test");
            error.setErrorLabels(std::vector{ErrorLabel::kTransientTransaction});
            return error.toBSON();
        }(),
        Microseconds{0},
        false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    ASSERT_THROWS_CODE(processor.onWriteBatchResponse(
                           opCtx,
                           routingCtx,
                           SimpleWriteBatchResponse{
                               {{shard1Name, ShardResponse::make(rcr1, {op1}, inTransaction)}}}),
                       DBException,
                       errorCode);
}

TEST_F(WriteBatchResponseProcessorTxnTest,
       NonTransientTransactionErrorInShardResponseHaltsProcessing) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(0, BSON("_id" << 2)),
                                            BulkWriteInsertOp(0, BSON("_id" << 3))},
                                           {NamespaceInfoEntry(nss1)});

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    auto reply = makeReply();
    reply.setNInserted(1);
    reply.setNMatched(0);
    reply.setNModified(0);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       BulkWriteReplyItem{0, Status::OK()},
                                                   },
                                                   nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    const auto errorCode = ErrorCodes::BadValue;
    const auto errorMsg = "CustomError";
    RemoteCommandResponse rcr2(
        host1,
        [&errorCode, &errorMsg] {
            auto error = ErrorReply(0, errorCode, errorMsg, errorMsg);
            return error.toBSON();
        }(),
        Microseconds{0},
        false);

    ShardId shard3Name = ShardId("shard3");

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)}, inTransaction)},
             {shard2Name, ShardResponse::make(rcr2, {WriteOp(request, 1)}, inTransaction)},
             {shard3Name, ShardResponse::makeEmpty({WriteOp(request, 2)})}}});

    // An error should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);

    // Confirm the generated bulk reply and batched command response are both correct.
    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNInserted(), 1);
    ASSERT_EQ(clientReply.getNMatched(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);
    ASSERT_EQ(clientReply.getNErrors(), 1);

    // Assert we don't return any results after the error.
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 2);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[0].getStatus(), Status::OK());
    ASSERT_EQ(batch[1].getStatus(), Status(errorCode, errorMsg));

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 1);
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    const auto errors = batchedCommandReply.getErrDetails();
    ASSERT_EQ(errors.size(), 1);
    ASSERT_EQ(errors[0].getIndex(), 1);
    ASSERT_EQ(errors[0].getStatus(), Status(errorCode, errorMsg));
}

TEST_F(WriteBatchResponseProcessorTxnTest, NonTransientTransactionErrorInReplyItemHaltsProcessing) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1)),
                                            BulkWriteInsertOp(0, BSON("_id" << 2)),
                                            BulkWriteInsertOp(0, BSON("_id" << 3))},
                                           {NamespaceInfoEntry(nss1)});

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    auto reply = makeReply();
    const auto errorCode = ErrorCodes::BadValue;
    const auto errorMsg = "CustomError";
    reply.setCursor(
        BulkWriteCommandResponseCursor(0,
                                       {
                                           BulkWriteReplyItem{0, Status::OK()},
                                           BulkWriteReplyItem{1, Status(errorCode, errorMsg)},
                                       },
                                       nss1));

    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name,
              ShardResponse::make(rcr1,
                                  {WriteOp(request, 0), WriteOp(request, 1), WriteOp(request, 2)},
                                  inTransaction)}}});

    // An error should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);

    // Confirm the generated bulk reply and batched command response are both correct.
    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNInserted(), 0);
    ASSERT_EQ(clientReply.getNMatched(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);

    // Assert we don't return any results after the error.
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 2);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[1].getIdx(), 1);
    ASSERT_EQ(batch[0].getStatus(), Status::OK());
    ASSERT_EQ(batch[1].getStatus(), Status(errorCode, errorMsg));

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 0);
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    const auto errors = batchedCommandReply.getErrDetails();
    ASSERT_EQ(errors.size(), 1);
    ASSERT_EQ(errors[0].getIndex(), 1);
    ASSERT_EQ(errors[0].getStatus(), Status(errorCode, errorMsg));
}

TEST_F(WriteBatchResponseProcessorTxnTest, RetryableErrorInReplyItemHaltsProcessing) {
    auto request = BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("_id" << 1))},
                                           {NamespaceInfoEntry(nss1)});

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);

    Status staleCollStatus(
        StaleConfigInfo(nss1, *shard1Endpoint.shardVersion, newShardVersion, shard1Name), "");

    auto reply = makeReply();
    reply.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, staleCollStatus}}, nss1));

    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{{{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)})}}});

    // An error should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);

    // Confirm the generated bulk reply and batched command response are both correct.
    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNInserted(), 0);
    ASSERT_EQ(clientReply.getNMatched(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);

    // Assert we don't return any results after the error.
    auto batch = clientReply.getCursor().getFirstBatch();
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[0].getStatus(), staleCollStatus);

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(batchedCommandReply.getN(), 0);
    ASSERT_EQ(batchedCommandReply.getNModified(), 0);
    const auto errors = batchedCommandReply.getErrDetails();
    ASSERT_EQ(errors.size(), 1);
    ASSERT_EQ(errors[0].getIndex(), 0);
    ASSERT_EQ(errors[0].getStatus(), staleCollStatus);
}

TEST_F(WriteBatchResponseProcessorTxnTest, ProcessorSetsRetriedStmtIdsInClientResponse) {
    auto request = BulkWriteCommandRequest(
        {BulkWriteInsertOp(0, BSON("_id" << 1)), BulkWriteInsertOp(0, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1)});

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    opCtx->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(opCtx);
    const bool inTransaction = true;

    auto reply1 = makeReply();
    reply1.setNInserted(1);
    reply1.setNMatched(0);
    reply1.setNModified(0);
    reply1.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));

    std::vector<StmtId> stmtIds1{0};
    reply1.setRetriedStmtIds(std::move(stmtIds1));

    auto reply2 = makeReply();
    reply2.setNInserted(1);
    reply2.setNMatched(0);
    reply2.setNModified(0);
    reply2.setCursor(
        BulkWriteCommandResponseCursor(0, {BulkWriteReplyItem{0, Status::OK()}}, nss1));

    std::vector<StmtId> stmtIds2{1};
    reply2.setRetriedStmtIds(std::move(stmtIds2));

    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply1.toBSON()), Microseconds{0}, false);
    RemoteCommandResponse rcr2(host2, setTopLevelOK(reply2.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {WriteOp(request, 0)}, inTransaction)},
             {shard2Name, ShardResponse::make(rcr2, {WriteOp(request, 1)}, inTransaction)}}});

    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);

    // Confirm the generated bulk reply and batched command response are both correct.
    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNInserted(), 2);
    ASSERT_EQ(clientReply.getNMatched(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);
    const auto retriedStmtIdsBulkWrite = clientReply.getRetriedStmtIds();
    ASSERT_EQ(retriedStmtIdsBulkWrite->size(), 2);

    // The stmtIds may be in any order.
    auto it = std::find(retriedStmtIdsBulkWrite->begin(), retriedStmtIdsBulkWrite->end(), 0);
    ASSERT_FALSE(it == retriedStmtIdsBulkWrite->end());
    it = std::find(retriedStmtIdsBulkWrite->begin(), retriedStmtIdsBulkWrite->end(), 1);
    ASSERT_FALSE(it == retriedStmtIdsBulkWrite->end());

    auto batchedCommandReply = processor.generateClientResponseForBatchedCommand(opCtx);
    ASSERT_EQ(*batchedCommandReply.getNOpt(), 2);
    ASSERT_FALSE(batchedCommandReply.getNModifiedOpt().has_value());
    ASSERT_FALSE(batchedCommandReply.isErrDetailsSet());
    const auto retriedStmtIdsBatchedWrite = clientReply.getRetriedStmtIds();
    ASSERT_EQ(retriedStmtIdsBatchedWrite->size(), 2);

    // The stmtIds may be in any order.
    it = std::find(retriedStmtIdsBatchedWrite->begin(), retriedStmtIdsBatchedWrite->end(), 0);
    ASSERT_FALSE(it == retriedStmtIdsBatchedWrite->end());
    it = std::find(retriedStmtIdsBatchedWrite->begin(), retriedStmtIdsBatchedWrite->end(), 1);
    ASSERT_FALSE(it == retriedStmtIdsBatchedWrite->end());
}

TEST_F(WriteBatchResponseProcessorTest, SimpleWriteErrorsOnlyModeNoError) {
    BulkWriteCommandRequest request = BulkWriteCommandRequest(
        {BulkWriteUpdateOp(0, BSON("_id" << 1), BSON("$set" << BSON("y" << 2))),
         BulkWriteInsertOp(0, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1)});

    const bool inTransaction = false;
    const bool errorsOnly = true;
    request.setErrorsOnly(errorsOnly);

    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);  // shard1, id: 0, OK
    WriteOp op2(request, 1);  // shard1, id: 0, OK

    auto reply = makeReply();
    reply.setNErrors(0);
    reply.setCursor(BulkWriteCommandResponseCursor(0,
                                                   {
                                                       BulkWriteReplyItem{0, Status::OK()},
                                                       BulkWriteReplyItem{1, Status::OK()},
                                                   },
                                                   nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {op1, op2}, inTransaction, errorsOnly)}}});
    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 0);
    ASSERT_EQ(clientReply.getNInserted(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);
    auto batch = clientReply.getCursor().getFirstBatch();
    // Assert that no responses were returned
    ASSERT_EQ(batch.size(), 0);
}

TEST_F(WriteBatchResponseProcessorTest, SimpleWriteErrorsOnlyModeWithError) {
    BulkWriteCommandRequest request = BulkWriteCommandRequest(
        {BulkWriteUpdateOp(0, BSON("_id" << 1), BSON("$set" << BSON("y" << 2))),
         BulkWriteInsertOp(0, BSON("_id" << 2))},
        {NamespaceInfoEntry(nss1)});

    const bool inTransaction = false;
    const bool errorsOnly = true;
    request.setErrorsOnly(errorsOnly);

    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);

    auto reply = makeReply();
    reply.setNErrors(1);
    reply.setCursor(
        BulkWriteCommandResponseCursor(0,
                                       {
                                           BulkWriteReplyItem{0, Status::OK()},
                                           BulkWriteReplyItem{1, Status(ErrorCodes::BadValue, "")},
                                       },
                                       nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {op1, op2}, inTransaction, errorsOnly)}}});
    // An error should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    ASSERT_EQ(clientReply.getNInserted(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);
    auto batch = clientReply.getCursor().getFirstBatch();
    // Assert that one error response was returned
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 1);
    ASSERT_EQ(batch[0].getStatus().code(), ErrorCodes::BadValue);
}

TEST_F(WriteBatchResponseProcessorTest, SimpleWriteErrorsOnlyModeUnordered) {
    BulkWriteCommandRequest request = BulkWriteCommandRequest(
        {BulkWriteUpdateOp(0, BSON("_id" << 1), BSON("$set" << BSON("y" << 2))),
         BulkWriteInsertOp(0, BSON("_id" << 2)),
         BulkWriteInsertOp(0, BSON("_id" << 3))},
        {NamespaceInfoEntry(nss1)});

    const bool inTransaction = false;
    const bool errorsOnly = true;
    request.setErrorsOnly(errorsOnly);
    request.setOrdered(false);

    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);
    WriteOp op2(request, 1);
    WriteOp op3(request, 2);

    auto reply = makeReply();
    reply.setNErrors(2);
    reply.setNInserted(0);
    reply.setCursor(
        BulkWriteCommandResponseCursor(0,
                                       {
                                           BulkWriteReplyItem{0, Status::OK()},
                                           BulkWriteReplyItem{1, Status(ErrorCodes::BadValue, "")},
                                           BulkWriteReplyItem{2, Status(ErrorCodes::BadValue, "")},
                                       },
                                       nss1));
    RemoteCommandResponse rcr1(host1, setTopLevelOK(reply.toBSON()), Microseconds{0}, false);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        SimpleWriteBatchResponse{
            {{shard1Name, ShardResponse::make(rcr1, {op1, op2, op3}, inTransaction, errorsOnly)}}});

    // Errors should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 2);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 2);
    ASSERT_EQ(clientReply.getNInserted(), 0);
    ASSERT_EQ(clientReply.getNModified(), 0);
    auto batch = clientReply.getCursor().getFirstBatch();
    // Assert that two error response were returned.
    ASSERT_EQ(batch.size(), 2);
    ASSERT_EQ(batch[0].getIdx(), 1);
    ASSERT_EQ(batch[0].getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(batch[1].getIdx(), 2);
    ASSERT_EQ(batch[1].getStatus().code(), ErrorCodes::BadValue);
}

TEST_F(WriteBatchResponseProcessorTest, TwoPhaseWriteErrorsOnlyModeNoError) {
    BulkWriteCommandRequest request = BulkWriteCommandRequest(
        {
            BulkWriteUpdateOp(0, BSON("_id" << 1), BSON("$set" << BSON("y" << 2))),
        },
        {NamespaceInfoEntry(nss1)});

    const bool inTransaction = false;
    const bool errorsOnly = true;
    request.setErrorsOnly(errorsOnly);

    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        NoRetryWriteBatchResponse::make(
            BSONObj(), /*wce*/ boost::none, op1, inTransaction, errorsOnly));

    // No errors.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 0);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 0);
    auto batch = clientReply.getCursor().getFirstBatch();
    // Assert that no error response was returned
    ASSERT_EQ(batch.size(), 0);
}

TEST_F(WriteBatchResponseProcessorTest, TwoPhaseWriteErrorsOnlyModeWithError) {
    BulkWriteCommandRequest request = BulkWriteCommandRequest(
        {
            BulkWriteUpdateOp(0, BSON("_id" << 1), BSON("$set" << BSON("y" << 2))),
        },
        {NamespaceInfoEntry(nss1)});

    const bool inTransaction = false;
    const bool errorsOnly = true;
    request.setErrorsOnly(errorsOnly);

    // Original id to shard request Id/status map.
    WriteOp op1(request, 0);

    WriteCommandRef cmdRef(request);
    Stats stats;
    WriteBatchResponseProcessor processor(cmdRef, stats);

    auto result = processor.onWriteBatchResponse(
        opCtx,
        routingCtx,
        NoRetryWriteBatchResponse::make(
            Status(ErrorCodes::BadValue, ""), /*wce*/ boost::none, op1, inTransaction, errorsOnly));

    // An error should have occurred.
    ASSERT_EQ(processor.getNumErrorsRecorded(), 1);
    ASSERT_EQ(result.opsToRetry.size(), 0);
    ASSERT_EQ(result.collsToCreate.size(), 0);

    auto clientReply = processor.generateClientResponseForBulkWriteCommand(opCtx);
    ASSERT_EQ(clientReply.getNErrors(), 1);
    auto batch = clientReply.getCursor().getFirstBatch();
    // Assert that one error response was returned
    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0].getIdx(), 0);
    ASSERT_EQ(batch[0].getStatus().code(), ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo::unified_write_executor
