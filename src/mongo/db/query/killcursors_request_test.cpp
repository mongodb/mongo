/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/db/query/killcursors_request.h"

#include "mongo/db/clientcursor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(KillCursorsRequestTest, parseFromBSONSuccess) {
    StatusWith<KillCursorsRequest> result =
        KillCursorsRequest::parseFromBSON("db",
                                          BSON("killCursors"
                                               << "coll"
                                               << "cursors"
                                               << BSON_ARRAY(CursorId(123) << CursorId(456))));
    ASSERT_OK(result.getStatus());
    KillCursorsRequest request = result.getValue();
    ASSERT_EQ(request.nss.ns(), "db.coll");
    ASSERT_EQ(request.cursorIds.size(), 2U);
    ASSERT_EQ(request.cursorIds[0], CursorId(123));
    ASSERT_EQ(request.cursorIds[1], CursorId(456));
}

TEST(KillCursorsRequestTest, parseFromBSONFirstFieldNotKillCursors) {
    StatusWith<KillCursorsRequest> result =
        KillCursorsRequest::parseFromBSON("db",
                                          BSON("foobar"
                                               << "coll"
                                               << "cursors"
                                               << BSON_ARRAY(CursorId(123) << CursorId(456))));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsRequestTest, parseFromBSONFirstFieldNotString) {
    StatusWith<KillCursorsRequest> result = KillCursorsRequest::parseFromBSON(
        "db", BSON("killCursors" << 99 << "cursors" << BSON_ARRAY(CursorId(123) << CursorId(456))));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsRequestTest, parseFromBSONInvalidNamespace) {
    StatusWith<KillCursorsRequest> result =
        KillCursorsRequest::parseFromBSON("",
                                          BSON("killCursors"
                                               << "coll"
                                               << "cursors"
                                               << BSON_ARRAY(CursorId(123) << CursorId(456))));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::InvalidNamespace);
}

TEST(KillCursorsRequestTest, parseFromBSONCursorFieldMissing) {
    StatusWith<KillCursorsRequest> result = KillCursorsRequest::parseFromBSON("db",
                                                                              BSON("killCursors"
                                                                                   << "coll"));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsRequestTest, parseFromBSONCursorFieldNotArray) {
    StatusWith<KillCursorsRequest> result =
        KillCursorsRequest::parseFromBSON("db",
                                          BSON("killCursors"
                                               << "coll"
                                               << "cursors"
                                               << CursorId(123)));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsRequestTest, parseFromBSONCursorFieldEmptyArray) {
    StatusWith<KillCursorsRequest> result =
        KillCursorsRequest::parseFromBSON("db",
                                          BSON("killCursors"
                                               << "coll"
                                               << "cursors"
                                               << BSONArrayBuilder().arr()));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
}


TEST(KillCursorsRequestTest, parseFromBSONCursorFieldContainsEltOfWrongType) {
    StatusWith<KillCursorsRequest> result =
        KillCursorsRequest::parseFromBSON("db",
                                          BSON("killCursors"
                                               << "coll"
                                               << "cursors"
                                               << BSON_ARRAY(CursorId(123) << "foo"
                                                                           << CursorId(456))));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsRequestTest, toBSON) {
    const NamespaceString nss("db.coll");
    std::vector<CursorId> cursorIds = {CursorId(123), CursorId(456)};
    KillCursorsRequest request(nss, cursorIds);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedObj = BSON("killCursors"
                               << "coll"
                               << "cursors"
                               << BSON_ARRAY(CursorId(123) << CursorId(456)));
    ASSERT_EQ(requestObj, expectedObj);
}

}  // namespace

}  // namespace mongo
