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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

const BSONObj kNestedOplog(BSON("$sessionMigrateInfo" << 1));

using WriteOpsRetryability = ServiceContextMongoDTest;

/**
 * Creates OplogEntry with given field values.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj oField,
                                boost::optional<BSONObj> o2Field = boost::none,
                                boost::optional<repl::OpTime> preImageOpTime = boost::none,
                                boost::optional<repl::OpTime> postImageOpTime = boost::none) {
    return {
        repl::DurableOplogEntry(opTime,                           // optime
                                opType,                           // opType
                                nss,                              // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                repl::OplogEntry::kOplogVersion,  // version
                                oField,                           // o
                                o2Field,                          // o2
                                {},                               // sessionInfo
                                boost::none,                      // upsert
                                Date_t(),                         // wall clock time
                                {},                               // statement ids
                                boost::none,     // optime of previous write within same transaction
                                preImageOpTime,  // pre-image optime
                                postImageOpTime,  // post-image optime
                                boost::none,      // ShardId of resharding recipient
                                boost::none,      // _id
                                boost::none)};    // needsRetryImage
}

void setUpReplication(ServiceContext* svcCtx) {
    auto replMock = std::make_unique<repl::ReplicationCoordinatorMock>(svcCtx);
    replMock->alwaysAllowWrites(true);
    repl::ReplicationCoordinator::set(svcCtx, std::move(replMock));
}

void setUpTxnParticipant(OperationContext* opCtx, std::vector<int> executedStmtIds) {
    const TxnNumber txnNumber = 1;
    opCtx->setTxnNumber(txnNumber);
    auto txnPart = TransactionParticipant::get(opCtx);
    txnPart.refreshFromStorageIfNeeded(opCtx);
    txnPart.beginOrContinue(opCtx, {txnNumber}, boost::none, boost::none);
    txnPart.addCommittedStmtIds(opCtx, std::move(executedStmtIds), repl::OpTime());
}

write_ops::FindAndModifyCommandRequest makeFindAndModifyRequest(
    NamespaceString fullNs, BSONObj query, boost::optional<write_ops::UpdateModification> update) {
    auto request = write_ops::FindAndModifyCommandRequest(fullNs);
    request.setQuery(query);
    if (update) {
        request.setUpdate(std::move(update));
    }
    return request;
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForUpdate) {
    const auto entry = assertGet(repl::OplogEntry::parse(
        BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "op"
                  << "u"
                  << "ns"
                  << "a.b"
                  << "wall" << Date_t() << "o" << BSON("_id" << 1 << "x" << 5) << "o2"
                  << BSON("_id" << 1))));

    auto res = parseOplogEntryForUpdate(entry);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 1);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForNestedUpdate) {
    auto innerOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),   // optime
                                     repl::OpTypeEnum::kUpdate,            // op type
                                     NamespaceString("a.b"),               // namespace
                                     BSON("_id" << 1 << "x" << 5),         // o
                                     BSON("_id" << 1));                    // o2
    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      NamespaceString("a.b"),              // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON());     // o2

    auto res = parseOplogEntryForUpdate(updateOplog);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 1);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForUpsert) {
    const auto entry = assertGet(repl::OplogEntry::parse(
        BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "op"
                  << "i"
                  << "ns"
                  << "a.b"
                  << "wall" << Date_t() << "o" << BSON("_id" << 1 << "x" << 5))));

    auto res = parseOplogEntryForUpdate(entry);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSON("_id" << 1));
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForNestedUpsert) {
    auto innerOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),   // optime
                                     repl::OpTypeEnum::kInsert,            // op type
                                     NamespaceString("a.b"),               // namespace
                                     BSON("_id" << 2));                    // o
    auto insertOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  /// optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      NamespaceString("a.b"),              // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON());     // o2

    auto res = parseOplogEntryForUpdate(insertOplog);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSON("_id" << 2));
}

TEST_F(WriteOpsRetryability, ShouldFailIfParsingDeleteOplogForUpdate) {
    auto deleteOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),  // optime
                                      repl::OpTypeEnum::kDelete,           // op type
                                      NamespaceString("a.b"),              // namespace
                                      BSON("_id" << 2));                   // o

    ASSERT_THROWS(parseOplogEntryForUpdate(deleteOplog), AssertionException);
}

TEST_F(WriteOpsRetryability, PerformInsertsSuccess) {
    auto opCtxRaii = makeOperationContext();
    // Use an unreplicated write block to avoid setting up more structures.
    repl::UnreplicatedWritesBlock unreplicated(opCtxRaii.get());
    setUpReplication(getServiceContext());

    write_ops::InsertCommandRequest insertOp(NamespaceString("foo.bar"));
    insertOp.getWriteCommandRequestBase().setOrdered(true);
    insertOp.setDocuments({BSON("_id" << 0), BSON("_id" << 1)});
    write_ops_exec::WriteResult result = write_ops_exec::performInserts(opCtxRaii.get(), insertOp);

    ASSERT_EQ(2, result.results.size());
    ASSERT_TRUE(result.results[0].isOK());
    ASSERT_TRUE(result.results[1].isOK());
}

TEST_F(WriteOpsRetryability, PerformRetryableInsertsSuccess) {
    auto opCtxRaii = makeOperationContext();
    opCtxRaii->setLogicalSessionId({UUID::gen(), {}});
    OperationContextSession session(opCtxRaii.get());
    // Use an unreplicated write block to avoid setting up more structures.
    repl::UnreplicatedWritesBlock unreplicated(opCtxRaii.get());
    setUpReplication(getServiceContext());

    // Set up a retryable write where statements 1 and 2 have already executed.
    setUpTxnParticipant(opCtxRaii.get(), {1, 2});

    write_ops::InsertCommandRequest insertOp(NamespaceString("foo.bar"));
    insertOp.getWriteCommandRequestBase().setOrdered(true);
    // Setup documents that cannot be successfully inserted to show that the retryable logic was
    // exercised.
    insertOp.setDocuments({BSON("_id" << 0), BSON("_id" << 0)});
    insertOp.getWriteCommandRequestBase().setStmtIds({{1, 2}});
    write_ops_exec::WriteResult result = write_ops_exec::performInserts(opCtxRaii.get(), insertOp);

    // Assert that both writes "succeeded". While there should have been a duplicate key error, the
    // `performInserts` obeyed the contract of not re-inserting a document that was declared to have
    // been inserted.
    ASSERT_EQ(2, result.results.size());
    ASSERT_TRUE(result.results[0].isOK());
    ASSERT_TRUE(result.results[1].isOK());
}

TEST_F(WriteOpsRetryability, PerformRetryableInsertsWithBatchedFailure) {
    auto opCtxRaii = makeOperationContext();
    opCtxRaii->setLogicalSessionId({UUID::gen(), {}});
    OperationContextSession session(opCtxRaii.get());
    // Use an unreplicated write block to avoid setting up more structures.
    repl::UnreplicatedWritesBlock unreplicated(opCtxRaii.get());
    setUpReplication(getServiceContext());

    // Set up a retryable write where statement 3 has already executed.
    setUpTxnParticipant(opCtxRaii.get(), {3});

    write_ops::InsertCommandRequest insertOp(NamespaceString("foo.bar"));
    insertOp.getWriteCommandRequestBase().setOrdered(false);
    // Setup documents such that the second will fail insertion.
    insertOp.setDocuments({BSON("_id" << 0), BSON("_id" << 0), BSON("_id" << 1)});
    insertOp.getWriteCommandRequestBase().setStmtIds({{1, 2, 3}});
    write_ops_exec::WriteResult result = write_ops_exec::performInserts(opCtxRaii.get(), insertOp);

    // Assert that the third (already executed) write succeeds, despite the second write failing
    // because this is an unordered insert.
    ASSERT_EQ(3, result.results.size());
    ASSERT_TRUE(result.results[0].isOK());
    ASSERT_FALSE(result.results[1].isOK());
    ASSERT_EQ(ErrorCodes::DuplicateKey, result.results[1].getStatus());
    ASSERT_TRUE(result.results[2].isOK());
}

TEST_F(WriteOpsRetryability, PerformOrderedInsertsStopsAtError) {
    auto opCtxRaii = makeOperationContext();
    opCtxRaii->setLogicalSessionId({UUID::gen(), {}});
    OperationContextSession session(opCtxRaii.get());
    // Use an unreplicated write block to avoid setting up more structures.
    repl::UnreplicatedWritesBlock unreplicated(opCtxRaii.get());
    setUpReplication(getServiceContext());

    write_ops::InsertCommandRequest insertOp(NamespaceString("foo.bar"));
    insertOp.getWriteCommandRequestBase().setOrdered(true);
    // Setup documents such that the second cannot be successfully inserted.
    insertOp.setDocuments({BSON("_id" << 0), BSON("_id" << 0), BSON("_id" << 1)});
    write_ops_exec::WriteResult result = write_ops_exec::performInserts(opCtxRaii.get(), insertOp);

    // Assert that the third write is not attempted because this is an ordered insert.
    ASSERT_EQ(2, result.results.size());
    ASSERT_TRUE(result.results[0].isOK());
    ASSERT_FALSE(result.results[1].isOK());
    ASSERT_EQ(ErrorCodes::DuplicateKey, result.results[1].getStatus());
}

TEST_F(WriteOpsRetryability, PerformOrderedInsertsStopsAtBadDoc) {
    auto opCtxRaii = makeOperationContext();
    opCtxRaii->setLogicalSessionId({UUID::gen(), {}});
    OperationContextSession session(opCtxRaii.get());
    // Use an unreplicated write block to avoid setting up more structures.
    repl::UnreplicatedWritesBlock unreplicated(opCtxRaii.get());
    setUpReplication(getServiceContext());

    write_ops::InsertCommandRequest insertOp(NamespaceString("foo.bar"));
    insertOp.getWriteCommandRequestBase().setOrdered(true);

    // Setup documents such that the second cannot be successfully inserted.
    auto largeBuffer = [](std::int32_t size) {
        std::vector<char> buffer(size);
        DataRange bufferRange(&buffer.front(), &buffer.back());
        ASSERT_OK(bufferRange.writeNoThrow(LittleEndian<int32_t>(size)));

        return buffer;
    }(17 * 1024 * 1024);

    insertOp.setDocuments({BSON("_id" << 0),
                           BSONObj(largeBuffer.data(), BSONObj::LargeSizeTrait{}),
                           BSON("_id" << 2)});
    write_ops_exec::WriteResult result = write_ops_exec::performInserts(opCtxRaii.get(), insertOp);

    // Assert that the third write is not attempted because this is an ordered insert.
    ASSERT_EQ(2, result.results.size());
    ASSERT_TRUE(result.results[0].isOK());
    ASSERT_FALSE(result.results[1].isOK());
    ASSERT_EQ(ErrorCodes::BadValue, result.results[1].getStatus());
}

TEST_F(WriteOpsRetryability, PerformUnorderedInsertsContinuesAtBadDoc) {
    auto opCtxRaii = makeOperationContext();
    opCtxRaii->setLogicalSessionId({UUID::gen(), {}});
    OperationContextSession session(opCtxRaii.get());
    // Use an unreplicated write block to avoid setting up more structures.
    repl::UnreplicatedWritesBlock unreplicated(opCtxRaii.get());
    setUpReplication(getServiceContext());

    write_ops::InsertCommandRequest insertOp(NamespaceString("foo.bar"));
    insertOp.getWriteCommandRequestBase().setOrdered(false);

    // Setup documents such that the second cannot be successfully inserted.
    auto largeBuffer = [](std::int32_t size) {
        std::vector<char> buffer(size);
        DataRange bufferRange(&buffer.front(), &buffer.back());
        ASSERT_OK(bufferRange.writeNoThrow(LittleEndian<int32_t>(size)));

        return buffer;
    }(17 * 1024 * 1024);

    insertOp.setDocuments({BSON("_id" << 0),
                           BSONObj(largeBuffer.data(), BSONObj::LargeSizeTrait{}),
                           BSON("_id" << 2)});
    write_ops_exec::WriteResult result = write_ops_exec::performInserts(opCtxRaii.get(), insertOp);

    // Assert that the third write is attempted because this is an unordered insert.
    ASSERT_EQ(3, result.results.size());
    ASSERT_TRUE(result.results[0].isOK());
    ASSERT_FALSE(result.results[1].isOK());
    ASSERT_TRUE(result.results[2].isOK());
    ASSERT_EQ(ErrorCodes::BadValue, result.results[1].getStatus());
}

using FindAndModifyRetryability = MockReplCoordServerFixture;

const NamespaceString kNs("test.user");

TEST_F(FindAndModifyRetryability, BasicUpsertReturnNew) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setUpsert(true);
    request.setNew(true);

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id"
                                           << "ID value"
                                           << "x" << 1));  // o

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, insertOplog).toBSON();
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject"
                           << BSON("n" << 1 << "updatedExisting" << false << "upserted"
                                       << "ID value")
                           << "value"
                           << BSON("_id"
                                   << "ID value"
                                   << "x" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, BasicUpsertReturnOld) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setUpsert(true);
    request.setNew(false);

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id"
                                           << "ID value"
                                           << "x" << 1));  // o

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, insertOplog).toBSON();
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject"
                           << BSON("n" << 1 << "updatedExisting" << false << "upserted"
                                       << "ID value")
                           << "value" << BSONNULL),
                      result);
}

TEST_F(FindAndModifyRetryability, NestedUpsert) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setUpsert(true);
    request.setNew(true);

    auto innerOplog = makeOplogEntry(repl::OpTime(),                       // optime
                                     repl::OpTypeEnum::kInsert,            // op type
                                     kNs,                                  // namespace
                                     BSON("_id" << 1));                    // o
    auto insertOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      kNs,                                 // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON());     // o2

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, insertOplog).toBSON();
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject"
                           << BSON("n" << 1 << "updatedExisting" << false << "upserted" << 1)
                           << "value" << BSON("_id" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, AttemptingToRetryUpsertWithUpdateWithoutUpsertErrors) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setUpsert(false);

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id" << 1));          // o

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, insertOplog).toBSON(),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPostImageButOplogHasPre) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                    // optime
                                    repl::OpTypeEnum::kNoop,        // op type
                                    kNs,                            // namespace
                                    BSON("_id" << 1 << "z" << 1));  // o

    insertOplogEntry(noteOplog);

    auto updateOplog = makeOplogEntry(repl::OpTime(),                // optime
                                      repl::OpTypeEnum::kUpdate,     // op type
                                      kNs,                           // namespace
                                      BSON("_id" << 1 << "y" << 1),  // o
                                      BSON("_id" << 1),              // o2
                                      imageOpTime,                   // pre-image optime
                                      boost::none);                  // post-image optime

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog).toBSON(),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsUpdateButOplogIsDelete) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                    // optime
                                    repl::OpTypeEnum::kNoop,        // op type
                                    kNs,                            // namespace
                                    BSON("_id" << 1 << "z" << 1));  // o

    insertOplogEntry(noteOplog);

    auto oplog = makeOplogEntry(repl::OpTime(),             // optime
                                repl::OpTypeEnum::kDelete,  // op type
                                kNs,                        // namespace
                                BSON("_id" << 1),           // o
                                boost::none,                // o2
                                imageOpTime,                // pre-image optime
                                boost::none);               // post-image optime

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, oplog).toBSON(),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPreImageButOplogHasPost) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(false);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                    // optime
                                    repl::OpTypeEnum::kNoop,        // op type
                                    kNs,                            // namespace
                                    BSON("_id" << 1 << "z" << 1));  // o

    insertOplogEntry(noteOplog);

    auto updateOplog = makeOplogEntry(repl::OpTime(),                // optime
                                      repl::OpTypeEnum::kUpdate,     // op type
                                      kNs,                           // namespace
                                      BSON("_id" << 1 << "y" << 1),  // o
                                      BSON("_id" << 1),              // o2
                                      boost::none,                   // pre-image optime
                                      imageOpTime);                  // post-image optime

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog).toBSON(),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, UpdateWithPreImage) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(false);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                    // optime
                                    repl::OpTypeEnum::kNoop,        // op type
                                    kNs,                            // namespace
                                    BSON("_id" << 1 << "z" << 1));  // o

    insertOplogEntry(noteOplog);

    auto updateOplog = makeOplogEntry(repl::OpTime(),                // optime
                                      repl::OpTypeEnum::kUpdate,     // op type
                                      kNs,                           // namespace
                                      BSON("_id" << 1 << "y" << 1),  // o
                                      BSON("_id" << 1),              // o2
                                      imageOpTime,                   // pre-image optime
                                      boost::none);                  // post-image optime

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, updateOplog).toBSON();
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value" << BSON("_id" << 1 << "z" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, NestedUpdateWithPreImage) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(false);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                    // optime
                                    repl::OpTypeEnum::kNoop,        // op type
                                    kNs,                            // namespace
                                    BSON("_id" << 1 << "z" << 1));  // o

    insertOplogEntry(noteOplog);

    auto innerOplog = makeOplogEntry(repl::OpTime(),                // optime
                                     repl::OpTypeEnum::kUpdate,     // op type
                                     kNs,                           // namespace
                                     BSON("_id" << 1 << "y" << 1),  // o
                                     BSON("_id" << 1));             // o2

    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // optype
                                      kNs,                                 // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON(),      // o2
                                      imageOpTime,                         // pre-image optime
                                      boost::none);                        // post-image optime

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, updateOplog).toBSON();
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value" << BSON("_id" << 1 << "z" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, UpdateRequestWithPreImageButNestedOpHasNoLinkShouldAssert) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(false);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                    // optime
                                    repl::OpTypeEnum::kNoop,        // op type
                                    kNs,                            // namespace
                                    BSON("_id" << 1 << "z" << 1));  // o

    insertOplogEntry(noteOplog);

    auto innerOplog = makeOplogEntry(repl::OpTime(),                // optime
                                     repl::OpTypeEnum::kUpdate,     // op type
                                     kNs,                           // namespace
                                     BSON("_id" << 1 << "y" << 1),  // o
                                     BSON("_id" << 1));             // o2

    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // optype
                                      kNs,                                 // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON(),      // o2
                                      boost::none,                         // pre-image optime
                                      boost::none);                        // post-image optime

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, UpdateWithPostImage) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                  // optime
                                    repl::OpTypeEnum::kNoop,      // op type
                                    kNs,                          // namespace
                                    BSON("a" << 1 << "b" << 1));  // o

    insertOplogEntry(noteOplog);

    auto updateOplog = makeOplogEntry(repl::OpTime(),                // optime
                                      repl::OpTypeEnum::kUpdate,     // op type
                                      kNs,                           // namespace
                                      BSON("_id" << 1 << "y" << 1),  // o
                                      BSON("_id" << 1),              // o2
                                      boost::none,                   // pre-image optime
                                      imageOpTime);                  // post-image optime

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, updateOplog).toBSON();
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value" << BSON("a" << 1 << "b" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, NestedUpdateWithPostImage) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                  // optime
                                    repl::OpTypeEnum::kNoop,      // op type
                                    kNs,                          // namespace
                                    BSON("a" << 1 << "b" << 1));  // o

    insertOplogEntry(noteOplog);

    auto innerOplog = makeOplogEntry(repl::OpTime(),                // optime
                                     repl::OpTypeEnum::kUpdate,     // op type
                                     kNs,                           // namespace
                                     BSON("_id" << 1 << "y" << 1),  // o
                                     BSON("_id" << 1));             // o2

    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      kNs,                                 // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON(),      // o2
                                      boost::none,                         // pre-image optime
                                      imageOpTime);                        // post-image optime

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, updateOplog).toBSON();
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value" << BSON("a" << 1 << "b" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, UpdateRequestWithPostImageButNestedOpHasNoLinkShouldAssert) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                  // optime
                                    repl::OpTypeEnum::kNoop,      // op type
                                    kNs,                          // namespace
                                    BSON("a" << 1 << "b" << 1));  // o

    insertOplogEntry(noteOplog);

    auto innerOplog = makeOplogEntry(repl::OpTime(),                // optime
                                     repl::OpTypeEnum::kUpdate,     // op type
                                     kNs,                           // namespace
                                     BSON("_id" << 1 << "y" << 1),  // o
                                     BSON("_id" << 1));             // o2

    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      kNs,                                 // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON(),      // o2
                                      boost::none,                         // pre-image optime
                                      boost::none);                        // post-image optime

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, UpdateWithPostImageButOplogDoesNotExistShouldError) {
    auto request = makeFindAndModifyRequest(
        kNs, BSONObj(), write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    request.setNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto updateOplog = makeOplogEntry(repl::OpTime(),                // optime
                                      repl::OpTypeEnum::kUpdate,     // op type
                                      kNs,                           // namespace
                                      BSON("_id" << 1 << "y" << 1),  // o
                                      BSON("_id" << 1),              // o2
                                      boost::none,                   // pre-image optime
                                      imageOpTime);                  // post-image optime

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog).toBSON(),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, BasicRemove) {
    auto request = makeFindAndModifyRequest(kNs, BSONObj(), boost::none);
    request.setRemove(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                     // optime
                                    repl::OpTypeEnum::kNoop,         // op type
                                    kNs,                             // namespace
                                    BSON("_id" << 20 << "a" << 1));  // o

    insertOplogEntry(noteOplog);

    auto removeOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kDelete,  // op type
                                      kNs,                        // namespace
                                      BSON("_id" << 20),          // o
                                      boost::none,                // o2
                                      imageOpTime,                // pre-image optime
                                      boost::none);               // post-image optime

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, removeOplog).toBSON();
    ASSERT_BSONOBJ_EQ(
        BSON("lastErrorObject" << BSON("n" << 1) << "value" << BSON("_id" << 20 << "a" << 1)),
        result);
}

TEST_F(FindAndModifyRetryability, NestedRemove) {
    auto request = makeFindAndModifyRequest(kNs, BSONObj(), boost::none);
    request.setRemove(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    auto noteOplog = makeOplogEntry(imageOpTime,                     // optime
                                    repl::OpTypeEnum::kNoop,         // op type
                                    kNs,                             // namespace
                                    BSON("_id" << 20 << "a" << 1));  // o

    insertOplogEntry(noteOplog);

    auto innerOplog = makeOplogEntry(repl::OpTime(),             // optime
                                     repl::OpTypeEnum::kDelete,  // op type
                                     kNs,                        // namespace
                                     BSON("_id" << 20));         // o

    auto removeOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      kNs,                                 // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.getEntry().toBSON(),      // o2
                                      imageOpTime,                         // pre-image optime
                                      boost::none);                        // post-image optime

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, removeOplog).toBSON();
    ASSERT_BSONOBJ_EQ(
        BSON("lastErrorObject" << BSON("n" << 1) << "value" << BSON("_id" << 20 << "a" << 1)),
        result);
}

TEST_F(FindAndModifyRetryability, AttemptingToRetryUpsertWithRemoveErrors) {
    auto request = makeFindAndModifyRequest(kNs, BSONObj(), boost::none);

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id" << 1));          // o

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, insertOplog).toBSON(),
                  AssertionException);
}

}  // namespace
}  // namespace mongo
