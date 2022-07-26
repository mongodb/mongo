/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/kill_cursors_gen.h"

#include "mongo/db/clientcursor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

const IDLParserContext ctxt("killCursors");

TEST(KillCursorsRequestTest, parseSuccess) {
    auto bsonObj = BSON("killCursors"
                        << "coll"
                        << "cursors" << BSON_ARRAY(CursorId(123) << CursorId(456)) << "$db"
                        << "db");
    KillCursorsCommandRequest request = KillCursorsCommandRequest::parse(ctxt, bsonObj);
    ASSERT_EQ(request.getNamespace().ns(), "db.coll");
    ASSERT_EQ(request.getCursorIds().size(), 2U);
    ASSERT_EQ(request.getCursorIds()[0], CursorId(123));
    ASSERT_EQ(request.getCursorIds()[1], CursorId(456));
}

TEST(KillCursorsRequestTest, parseCursorsFieldEmptyArray) {
    auto bsonObj = BSON("killCursors"
                        << "coll"
                        << "cursors" << BSONArray() << "$db"
                        << "db");
    KillCursorsCommandRequest request = KillCursorsCommandRequest::parse(ctxt, bsonObj);
    ASSERT_EQ(request.getCursorIds().size(), 0U);
}

TEST(KillCursorsRequestTest, parseFirstFieldNotString) {
    auto bsonObj =
        BSON("killCursors" << 99 << "cursors" << BSON_ARRAY(CursorId(123) << CursorId(456)) << "$db"
                           << "db");
    ASSERT_THROWS_CODE(
        KillCursorsCommandRequest::parse(ctxt, bsonObj), AssertionException, ErrorCodes::BadValue);
}

TEST(KillCursorsRequestTest, parseInvalidNamespace) {
    auto bsonObj = BSON("killCursors"
                        << "coll"
                        << "cursors" << BSON_ARRAY(CursorId(123) << CursorId(456)));
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(ctxt, bsonObj), AssertionException, 40414);
}

TEST(KillCursorsRequestTest, parseCursorsFieldMissing) {
    auto bsonObj = BSON("killCursors"
                        << "coll"
                        << "$db"
                        << "db");
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(ctxt, bsonObj), AssertionException, 40414);
}

TEST(KillCursorsRequestTest, parseCursorFieldNotArray) {
    auto bsonObj = BSON("killCursors"
                        << "coll"
                        << "cursors" << CursorId(123) << "$db"
                        << "db");
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(ctxt, bsonObj),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(KillCursorsRequestTest, parseCursorFieldArrayWithNonCursorIdValue) {
    auto bsonObj = BSON("killCursors"
                        << "coll"
                        << "cursors" << BSON_ARRAY(CursorId(123) << "String value") << "$db"
                        << "db");
    ASSERT_THROWS_CODE(KillCursorsCommandRequest::parse(ctxt, bsonObj),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(KillCursorsRequestTest, toBSON) {
    const NamespaceString nss("db.coll");
    std::vector<CursorId> cursorIds = {CursorId(123)};
    KillCursorsCommandRequest request(nss, cursorIds);
    BSONObj requestObj = request.toBSON(BSONObj{});
    BSONObj expectedObj = BSON("killCursors"
                               << "coll"
                               << "cursors" << BSON_ARRAY(CursorId(123)));
    ASSERT_BSONOBJ_EQ(requestObj, expectedObj);
}

}  // namespace
}  // namespace mongo
