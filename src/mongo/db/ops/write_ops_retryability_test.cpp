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
    return repl::OplogEntry(opTime,                           // optime
                            0,                                // hash
                            opType,                           // opType
                            nss,                              // namespace
                            boost::none,                      // uuid
                            boost::none,                      // fromMigrate
                            repl::OplogEntry::kOplogVersion,  // version
                            oField,                           // o
                            o2Field,                          // o2
                            {},                               // sessionInfo
                            boost::none,                      // upsert
                            boost::none,                      // wall clock time
                            boost::none,                      // statement id
                            boost::none,       // optime of previous write within same transaction
                            preImageOpTime,    // pre-image optime
                            postImageOpTime);  // post-image optime
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
    auto innerOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),   // optime
                                     repl::OpTypeEnum::kUpdate,            // op type
                                     NamespaceString("a.b"),               // namespace
                                     BSON("_id" << 1 << "x" << 5),         // o
                                     BSON("_id" << 1));                    // o2
    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      NamespaceString("a.b"),              // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.toBSON());                // o2

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
    auto innerOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),   // optime
                                     repl::OpTypeEnum::kInsert,            // op type
                                     NamespaceString("a.b"),               // namespace
                                     BSON("_id" << 2));                    // o
    auto insertOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  /// optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      NamespaceString("a.b"),              // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.toBSON());                // o2

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

TEST_F(FindAndModifyRetryability, BasicUpsertReturnNew) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(true);
    request.setShouldReturnNew(true);

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id"
                                           << "ID value"
                                           << "x"
                                           << 1));  // o

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

TEST_F(FindAndModifyRetryability, BasicUpsertReturnOld) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(true);
    request.setShouldReturnNew(false);

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id"
                                           << "ID value"
                                           << "x"
                                           << 1));  // o

    auto result = constructFindAndModifyRetryResult(opCtx(), request, insertOplog);
    ASSERT_BSONOBJ_EQ(BSON("lastErrorObject"
                           << BSON("n" << 1 << "updatedExisting" << false << "upserted"
                                       << "ID value")
                           << "value"
                           << BSONNULL),
                      result);
}

TEST_F(FindAndModifyRetryability, NestedUpsert) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setUpsert(true);
    request.setShouldReturnNew(true);

    auto innerOplog = makeOplogEntry(repl::OpTime(),                       // optime
                                     repl::OpTypeEnum::kInsert,            // op type
                                     kNs,                                  // namespace
                                     BSON("_id" << 1));                    // o
    auto insertOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kNoop,             // op type
                                      kNs,                                 // namespace
                                      kNestedOplog,                        // o
                                      innerOplog.toBSON());                // o2

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

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id" << 1));          // o

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, insertOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPostImageButOplogHasPre) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

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

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsUpdateButOplogIsDelete) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(true);

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

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, oplog), AssertionException);
}

TEST_F(FindAndModifyRetryability, ErrorIfRequestIsPreImageButOplogHasPost) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(false);

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

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, UpdateWithPreImage) {
    auto request = FindAndModifyRequest::makeUpdate(kNs, BSONObj(), BSONObj());
    request.setShouldReturnNew(false);

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
                                      innerOplog.toBSON(),                 // o2
                                      imageOpTime,                         // pre-image optime
                                      boost::none);                        // post-image optime

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
                                      innerOplog.toBSON(),                 // o2
                                      boost::none,                         // pre-image optime
                                      imageOpTime);                        // post-image optime

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
    auto updateOplog = makeOplogEntry(repl::OpTime(),                // optime
                                      repl::OpTypeEnum::kUpdate,     // op type
                                      kNs,                           // namespace
                                      BSON("_id" << 1 << "y" << 1),  // o
                                      BSON("_id" << 1),              // o2
                                      boost::none,                   // pre-image optime
                                      imageOpTime);                  // post-image optime

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, updateOplog),
                  AssertionException);
}

TEST_F(FindAndModifyRetryability, BasicRemove) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

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

    auto result = constructFindAndModifyRetryResult(opCtx(), request, removeOplog);
    ASSERT_BSONOBJ_EQ(
        BSON("lastErrorObject" << BSON("n" << 1) << "value" << BSON("_id" << 20 << "a" << 1)),
        result);
}

TEST_F(FindAndModifyRetryability, NestedRemove) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

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
                                      innerOplog.toBSON(),                 // o2
                                      imageOpTime,                         // pre-image optime
                                      boost::none);                        // post-image optime

    auto result = constructFindAndModifyRetryResult(opCtx(), request, removeOplog);
    ASSERT_BSONOBJ_EQ(
        BSON("lastErrorObject" << BSON("n" << 1) << "value" << BSON("_id" << 20 << "a" << 1)),
        result);
}

TEST_F(FindAndModifyRetryability, AttemptingToRetryUpsertWithRemoveErrors) {
    auto request = FindAndModifyRequest::makeRemove(kNs, BSONObj());

    auto insertOplog = makeOplogEntry(repl::OpTime(),             // optime
                                      repl::OpTypeEnum::kInsert,  // op type
                                      kNs,                        // namespace
                                      BSON("_id" << 1));          // o

    ASSERT_THROWS(constructFindAndModifyRetryResult(opCtx(), request, insertOplog),
                  AssertionException);
}

}  // namespace
}  // namespace mongo
