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
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/repl/oplog_entry.h"
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

}  // namespace
}  // namespace mongo
