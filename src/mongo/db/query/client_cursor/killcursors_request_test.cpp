// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <vector>

namespace mongo {

namespace {

const IDLParserContext ctxt("killCursors");

TEST(KillCursorsRequestTest, parseSuccess) {
    auto bsonObj =
        BSON("killCursors" << "coll"
                           << "cursors" << BSON_ARRAY(CursorId(123) << CursorId(456)) << "$db"
                           << "db");
    KillCursorsCommandRequest request = KillCursorsCommandRequest::parse(bsonObj, ctxt);
    ASSERT_EQ(request.getNamespace().ns_forTest(), "db.coll");
    ASSERT_EQ(request.getCursorIds().size(), 2U);
    ASSERT_EQ(request.getCursorIds()[0], CursorId(123));
    ASSERT_EQ(request.getCursorIds()[1], CursorId(456));
}

TEST(KillCursorsRequestTest, parseCursorsFieldEmptyArray) {
    auto bsonObj = BSON("killCursors" << "coll"
                                      << "cursors" << BSONArray() << "$db"
                                      << "db");
    KillCursorsCommandRequest request = KillCursorsCommandRequest::parse(bsonObj, ctxt);
    ASSERT_EQ(request.getCursorIds().size(), 0U);
}

TEST(KillCursorsRequestTest, parseFirstFieldNotString) {
    auto bsonObj =
        BSON("killCursors" << 99 << "cursors" << BSON_ARRAY(CursorId(123) << CursorId(456)) << "$db"
                           << "db");
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(bsonObj, ctxt),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST(KillCursorsRequestTest, parseInvalidNamespace) {
    auto bsonObj = BSON("killCursors" << "coll"
                                      << "cursors" << BSON_ARRAY(CursorId(123) << CursorId(456)));
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(bsonObj, ctxt),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(KillCursorsRequestTest, parseCursorsFieldMissing) {
    auto bsonObj = BSON("killCursors" << "coll"
                                      << "$db"
                                      << "db");
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(bsonObj, ctxt),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(KillCursorsRequestTest, parseCursorFieldNotArray) {
    auto bsonObj = BSON("killCursors" << "coll"
                                      << "cursors" << CursorId(123) << "$db"
                                      << "db");
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(bsonObj, ctxt),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(KillCursorsRequestTest, parseCursorFieldArrayWithNonCursorIdValue) {
    auto bsonObj =
        BSON("killCursors" << "coll"
                           << "cursors" << BSON_ARRAY(CursorId(123) << "String value") << "$db"
                           << "db");
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(bsonObj, ctxt),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(KillCursorsRequestTest, toBSON) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("db.coll");
    std::vector<CursorId> cursorIds = {CursorId(123)};
    KillCursorsCommandRequest request(nss, cursorIds);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedObj = BSON("killCursors" << "coll"
                                             << "cursors" << BSON_ARRAY(CursorId(123)));
    ASSERT_BSONOBJ_EQ(requestObj, expectedObj);
}

}  // namespace
}  // namespace mongo
