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
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

const BSONObj kNestedOplog(BSON("$sessionMigrateInfo" << 1));

using WriteOpsRetryability = ServiceContextMongoDTest;

TEST_F(WriteOpsRetryability, ParseOplogEntryForInsert) {
    const auto entry = assertGet(
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "i"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5))));

    auto res = parseOplogEntryForInsert(entry);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForNestedInsert) {
    repl::OplogEntry innerOplog(repl::OpTime(Timestamp(50, 10), 1),
                                0,
                                repl::OpTypeEnum::kInsert,
                                NamespaceString("a.b"),
                                BSON("_id" << 2));
    repl::OplogEntry insertOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 NamespaceString("a.b"),
                                 kNestedOplog,
                                 innerOplog.toBSON());

    auto res = parseOplogEntryForInsert(insertOplog);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForUpdate) {
    const auto entry = assertGet(
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "u"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5)
                                          << "o2"
                                          << BSON("_id" << 1))));

    auto res = parseOplogEntryForUpdate(entry);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 1);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForNestedUpdate) {
    repl::OplogEntry innerOplog(repl::OpTime(Timestamp(50, 10), 1),
                                0,
                                repl::OpTypeEnum::kUpdate,
                                NamespaceString("a.b"),
                                BSON("_id" << 1 << "x" << 5),
                                BSON("_id" << 1));
    repl::OplogEntry updateOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 NamespaceString("a.b"),
                                 kNestedOplog,
                                 innerOplog.toBSON());

    auto res = parseOplogEntryForUpdate(updateOplog);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 1);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForUpsert) {
    const auto entry = assertGet(
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "i"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5))));

    auto res = parseOplogEntryForUpdate(entry);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSON("_id" << 1));
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForNestedUpsert) {
    repl::OplogEntry innerOplog(repl::OpTime(Timestamp(50, 10), 1),
                                0,
                                repl::OpTypeEnum::kInsert,
                                NamespaceString("a.b"),
                                BSON("_id" << 2));
    repl::OplogEntry insertOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 NamespaceString("a.b"),
                                 kNestedOplog,
                                 innerOplog.toBSON());

    auto res = parseOplogEntryForUpdate(insertOplog);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSON("_id" << 2));
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForDelete) {
    const auto entry = assertGet(
        repl::OplogEntry::parse(BSON("ts" << Timestamp(50, 10) << "t" << 1LL << "h" << 0LL << "op"
                                          << "d"
                                          << "ns"
                                          << "a.b"
                                          << "o"
                                          << BSON("_id" << 1 << "x" << 5))));

    auto res = parseOplogEntryForDelete(entry);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ParseOplogEntryForNestedDelete) {
    repl::OplogEntry innerOplog(repl::OpTime(Timestamp(50, 10), 1),
                                0,
                                repl::OpTypeEnum::kDelete,
                                NamespaceString("a.b"),
                                BSON("_id" << 2));
    repl::OplogEntry deleteOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 NamespaceString("a.b"),
                                 kNestedOplog,
                                 innerOplog.toBSON());

    auto res = parseOplogEntryForDelete(deleteOplog);

    ASSERT_EQ(res.getN(), 1);
    ASSERT_EQ(res.getNModified(), 0);
    ASSERT_BSONOBJ_EQ(res.getUpsertedId(), BSONObj());
}

TEST_F(WriteOpsRetryability, ShouldFailIfParsingDeleteOplogForInsert) {
    repl::OplogEntry deleteOplog(repl::OpTime(Timestamp(50, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kDelete,
                                 NamespaceString("a.b"),
                                 BSON("_id" << 2));

    ASSERT_THROWS(parseOplogEntryForInsert(deleteOplog), AssertionException);
}

TEST_F(WriteOpsRetryability, ShouldFailIfParsingDeleteOplogForUpdate) {
    repl::OplogEntry deleteOplog(repl::OpTime(Timestamp(50, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kDelete,
                                 NamespaceString("a.b"),
                                 BSON("_id" << 2));

    ASSERT_THROWS(parseOplogEntryForUpdate(deleteOplog), AssertionException);
}

TEST_F(WriteOpsRetryability, ShouldFailIfParsingInsertOplogForDelete) {
    repl::OplogEntry insertOplog(repl::OpTime(Timestamp(50, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kInsert,
                                 NamespaceString("a.b"),
                                 BSON("_id" << 2));

    ASSERT_THROWS(parseOplogEntryForDelete(insertOplog), AssertionException);
}

class FindAndModifyRetryability : public MockReplCoordServerFixture {
public:
    FindAndModifyRetryability() = default;

protected:
    /**
     * Helper function to return a fully-constructed BSONObj instead of having to use
     * BSONObjBuilder.
     */
    static BSONObj constructFindAndModifyRetryResult(OperationContext* opCtx,
                                                     const FindAndModifyRequest& request,
                                                     const repl::OplogEntry& oplogEntry) {
        BSONObjBuilder builder;
        parseOplogEntryForFindAndModify(opCtx, request, oplogEntry, &builder);
        return builder.obj();
    }
};

const NamespaceString kNs("test.user");

TEST_F(FindAndModifyRetryability, BasicUpsert) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(true);

    repl::OplogEntry insertOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kInsert,
                                 kNs,
                                 BSON("_id"
                                      << "ID value"
                                      << "x"
                                      << 1));

    auto result = constructFindAndModifyRetryResult(opCtx(), request, insertOplog);
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject"
                           << BSON("n" << 1 << "updatedExisting" << false << "upserted"
                                       << "ID value")
                           << "value"
                           << BSON("_id"
                                   << "ID value"
                                   << "x"
                                   << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, NestedUpsert) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(true);

    repl::OplogEntry innerOplog(
        repl::OpTime(), 0, repl::OpTypeEnum::kInsert, kNs, BSON("_id" << 1));
    repl::OplogEntry insertOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 kNs,
                                 kNestedOplog,
                                 innerOplog.toBSON());

    auto result = constructFindAndModifyRetryResult(opCtx(), request, insertOplog);
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject"
                           << BSON("n" << 1 << "updatedExisting" << false << "upserted" << 1)
                           << "value"
                           << BSON("_id" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, AttemptingToRetryUpsertWithUpdateWithoutUpsertErrors) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(false);

    repl::OplogEntry insertOplog(
        repl::OpTime(), 0, repl::OpTypeEnum::kInsert, kNs, BSON("_id" << 1));

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, insertOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPostImageButOplogHasPre) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("_id" << 1 << "y" << 1),
                                 BSON("_id" << 1));
    updateOplog.setPreImageOpTime(imageOpTime);

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsUpdateButOplogIsDelete) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry oplog(repl::OpTime(), 0, repl::OpTypeEnum::kDelete, kNs, BSON("_id" << 1));
    oplog.setPreImageOpTime(imageOpTime);

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, oplog), AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPreImageButOplogHasPost) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(false);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("_id" << 1 << "y" << 1),
                                 BSON("_id" << 1));
    updateOplog.setPostImageOpTime(imageOpTime);

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, UpdateWithPreImage) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(false);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("_id" << 1 << "y" << 1),
                                 BSON("_id" << 1));
    updateOplog.setPreImageOpTime(imageOpTime);

    auto result = constructFindAndModifyRetryResult(opCtx(), request, updateOplog);
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value"
                                             << BSON("_id" << 1 << "z" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, NestedUpdateWithPreImage) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(false);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 1 << "z" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry innerOplog(repl::OpTime(),
                                0,
                                repl::OpTypeEnum::kUpdate,
                                kNs,
                                BSON("_id" << 1 << "y" << 1),
                                BSON("_id" << 1));

    repl::OplogEntry updateOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 kNs,
                                 kNestedOplog,
                                 innerOplog.toBSON());
    updateOplog.setPreImageOpTime(imageOpTime);

    auto result = constructFindAndModifyRetryResult(opCtx(), request, updateOplog);
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value"
                                             << BSON("_id" << 1 << "z" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, UpdateWithPostImage) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("a" << 1 << "b" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("_id" << 1 << "y" << 1),
                                 BSON("_id" << 1));
    updateOplog.setPostImageOpTime(imageOpTime);

    auto result = constructFindAndModifyRetryResult(opCtx(), request, updateOplog);
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value"
                                             << BSON("a" << 1 << "b" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, NestedUpdateWithPostImage) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("a" << 1 << "b" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry innerOplog(repl::OpTime(),
                                0,
                                repl::OpTypeEnum::kUpdate,
                                kNs,
                                BSON("_id" << 1 << "y" << 1),
                                BSON("_id" << 1));

    repl::OplogEntry updateOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 kNs,
                                 kNestedOplog,
                                 innerOplog.toBSON());
    updateOplog.setPostImageOpTime(imageOpTime);

    auto result = constructFindAndModifyRetryResult(opCtx(), request, updateOplog);
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject" << BSON("n" << 1 << "updatedExisting" << true)
                                             << "value"
                                             << BSON("a" << 1 << "b" << 1)),
                      result);
}

TEST_F(FindAndModifyRetryability, UpdateWithPostImageButOplogDoesNotExistShouldError) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry updateOplog(repl::OpTime(),
                                 0,
                                 repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("_id" << 1 << "y" << 1),
                                 BSON("_id" << 1));
    updateOplog.setPostImageOpTime(imageOpTime);

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, BasicRemove) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 20 << "a" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry removeOplog(
        repl::OpTime(), 0, repl::OpTypeEnum::kDelete, kNs, BSON("_id" << 20));
    removeOplog.setPreImageOpTime(imageOpTime);

    auto result = constructFindAndModifyRetryResult(opCtx(), request, removeOplog);
    ASSERT_BSONOBJ_EQ(
        BSON("lastErrorObject" << BSON("n" << 1) << "value" << BSON("_id" << 20 << "a" << 1)),
        result);
}

TEST_F(FindAndModifyRetryability, NestedRemove) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

    repl::OpTime imageOpTime(Timestamp(120, 3), 1);
    repl::OplogEntry noteOplog(
        imageOpTime, 0, repl::OpTypeEnum::kNoop, kNs, BSON("_id" << 20 << "a" << 1));

    insertOplogEntry(noteOplog);

    repl::OplogEntry innerOplog(
        repl::OpTime(), 0, repl::OpTypeEnum::kDelete, kNs, BSON("_id" << 20));

    repl::OplogEntry removeOplog(repl::OpTime(Timestamp(60, 10), 1),
                                 0,
                                 repl::OpTypeEnum::kNoop,
                                 kNs,
                                 kNestedOplog,
                                 innerOplog.toBSON());
    removeOplog.setPreImageOpTime(imageOpTime);

    auto result = constructFindAndModifyRetryResult(opCtx(), request, removeOplog);
    ASSERT_BSONOBJ_EQ(
        BSON("lastErrorObject" << BSON("n" << 1) << "value" << BSON("_id" << 20 << "a" << 1)),
        result);
}

TEST_F(FindAndModifyRetryability, AttemptingToRetryUpsertWithRemoveErrors) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

    repl::OplogEntry insertOplog(
        repl::OpTime(), 0, repl::OpTypeEnum::kInsert, kNs, BSON("_id" << 1));

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, insertOplog),
                  AssertionException);
}

}  // namespace
}  // namespace mongo
