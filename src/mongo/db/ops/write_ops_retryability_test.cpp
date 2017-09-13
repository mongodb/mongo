/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using WriteOpsRetryability = ServiceContextMongoDTest;

TEST_F(WriteOpsRetryability, ParseOplogEntryForInsert) {
    auto entry =
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "i"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5)));
    ASSERT(entry.isOK());

    auto res = mongo::parseOplogEntryForInsert(entry.getValue());

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForUpdate) {
    auto entry =
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "u"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5)
                                          << "o2"
                                          << BSON("_id" << 1)));
    ASSERT(entry.isOK());

    auto res = mongo::parseOplogEntryForUpdate(entry.getValue());

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 1);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForUpsert) {
    auto entry =
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "i"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5)));
    ASSERT(entry.isOK());

    auto res = mongo::parseOplogEntryForUpdate(entry.getValue());

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSON("_id" << 1));
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForDelete) {
    auto entry =
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "d"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5)));
    ASSERT(entry.isOK());

    auto res = mongo::parseOplogEntryForDelete(entry.getValue());

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

using FindAndModifyRetryability = MockReplCoordServerFixture;

NamespaceString kNs("test.user");

TEST_F(FindAndModifyRetryability, BasicUpsert) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(true);

    repl::OplogEntry insertOplog(repl::OpTime(), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1));

    auto result = parseOplogEntryForFindAndModify(nullptr, request, insertOplog);

    auto lastError = result.getLastErrorObject();
    ASSERT_EQ(1, lastError.getN());
    ASSERT_TRUE(lastError.getUpdatedExisting());
    ASSERT_FALSE(lastError.getUpdatedExisting().value());

    ASSERT_BSONOBJ_EQ(BSON("x" << 1), result.getValue());
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsUpsertButOplogIsUpdate) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(true);

    Timestamp imageTs(120, 3);
    repl::OplogEntry noteOplog(
        repl::OpTime(imageTs, 1), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry oplog(
        repl::OpTime(), 0, repl::OpTypeEnum::kUpdate, kNs, BSON("x" << 1), BSON("y" << 1));
    oplog.setPreImageTs(imageTs);

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, oplog), AssertionException);
}

TEST_F(FindAndModifyRetryability, AttemptingToRetryUpsertWithUpdateWithoutUpsertErrors) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(false);

    repl::OplogEntry insertOplog(repl::OpTime(), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1));

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, insertOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPostImageButOplogHasPre) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    Timestamp imageTs(120, 3);
    repl::OplogEntry noteOplog(
        repl::OpTime(imageTs, 1), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("x" << 1 << "y" << 1),
                                 BSON("x" << 1));
    updateOplog.setPreImageTs(imageTs);

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsUpdateButOplogIsDelete) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    Timestamp imageTs(120, 3);
    repl::OplogEntry noteOplog(
        repl::OpTime(imageTs, 1), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry oplog(repl::OpTime(), 0, repl::OpTypeEnum::kDelete, kNs, BSON("_id" << 1));
    oplog.setPreImageTs(imageTs);

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, oplog), AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPreImageButOplogHasPost) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(false);

    Timestamp imageTs(120, 3);
    repl::OplogEntry noteOplog(
        repl::OpTime(imageTs, 1), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("x" << 1 << "y" << 1),
                                 BSON("x" << 1));
    updateOplog.setPostImageTs(imageTs);

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, UpdateWithPreImage) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(false);

    Timestamp imageTs(120, 3);
    repl::OplogEntry noteOplog(
        repl::OpTime(imageTs, 1), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("x" << 1 << "y" << 1),
                                 BSON("x" << 1));
    updateOplog.setPreImageTs(imageTs);

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, updateOplog);

    auto lastError = result.getLastErrorObject();
    ASSERT_EQ(1, lastError.getN());
    ASSERT_TRUE(lastError.getUpdatedExisting());
    ASSERT_TRUE(lastError.getUpdatedExisting().value());

    ASSERT_BSONOBJ_EQ(BSON("x" << 1 << "z" << 1), result.getValue());
}

TEST_F(FindAndModifyRetryability, UpdateWithPostImage) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    Timestamp imageTs(120, 3);
    repl::OplogEntry noteOplog(
        repl::OpTime(imageTs, 1), 0, repl::OpTypeEnum::kNoop, kNs, BSON("a" << 1 << "b" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("x" << 1 << "y" << 1),
                                 BSON("x" << 1));
    updateOplog.setPostImageTs(imageTs);

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, updateOplog);

    auto lastError = result.getLastErrorObject();
    ASSERT_EQ(1, lastError.getN());
    ASSERT_TRUE(lastError.getUpdatedExisting());
    ASSERT_TRUE(lastError.getUpdatedExisting().value());

    ASSERT_BSONOBJ_EQ(BSON("a" << 1 << "b" << 1), result.getValue());
}

TEST_F(FindAndModifyRetryability, UpdateWithPostImageButOplogDoesNotExistShouldError) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    Timestamp imageTs(120, 3);
    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("x" << 1 << "y" << 1),
                                 BSON("x" << 1));
    updateOplog.setPostImageTs(imageTs);

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, BasicRemove) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

    Timestamp imageTs(120, 3);
    repl::OplogEntry noteOplog(
        repl::OpTime(imageTs, 1), 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 20 << "a" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry removeOplog(
        repl::OpTime(), 0, repl::OpTypeEnum::kDelete, kNs, BSON("_id" << 20));
    removeOplog.setPreImageTs(imageTs);

    auto result = parseOplogEntryForFindAndModify(opCtx(), request, removeOplog);

    auto lastError = result.getLastErrorObject();
    ASSERT_EQ(1, lastError.getN());
    ASSERT_FALSE(lastError.getUpdatedExisting());

    ASSERT_BSONOBJ_EQ(BSON("_id" << 20 << "a" << 1), result.getValue());
}

TEST_F(FindAndModifyRetryability, AttemptingToRetryUpsertWithRemoveErrors) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

    repl::OplogEntry insertOplog(repl::OpTime(), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1));

    ASSERT_THROWS(parseOplogEntryForFindAndModify(opCtx(), request, insertOplog),
                  AssertionException);
}

}  // namespace
}  // namespace mongo
