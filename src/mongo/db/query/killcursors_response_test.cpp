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

#include "mongo/db/query/killcursors_response.h"

#include "mongo/db/clientcursor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(KillCursorsResponseTest, parseFromBSONSuccess) {
    StatusWith<KillCursorsResponse> result = KillCursorsResponse::parseFromBSON(
        BSON("cursorsKilled" << BSON_ARRAY(CursorId(123)) << "cursorsNotFound"
                             << BSON_ARRAY(CursorId(456) << CursorId(6))
                             << "cursorsAlive"
                             << BSON_ARRAY(CursorId(7) << CursorId(8) << CursorId(9))
                             << "cursorsUnknown"
                             << BSONArray()
                             << "ok"
                             << 1.0));
    ASSERT_OK(result.getStatus());
    KillCursorsResponse response = result.getValue();
    ASSERT_EQ(response.cursorsKilled.size(), 1U);
    ASSERT_EQ(response.cursorsKilled[0], CursorId(123));
    ASSERT_EQ(response.cursorsNotFound.size(), 2U);
    ASSERT_EQ(response.cursorsNotFound[0], CursorId(456));
    ASSERT_EQ(response.cursorsNotFound[1], CursorId(6));
    ASSERT_EQ(response.cursorsAlive.size(), 3U);
    ASSERT_EQ(response.cursorsAlive[0], CursorId(7));
    ASSERT_EQ(response.cursorsAlive[1], CursorId(8));
    ASSERT_EQ(response.cursorsAlive[2], CursorId(9));
    ASSERT_EQ(response.cursorsUnknown.size(), 0U);
}

TEST(KillCursorsResponseTest, parseFromBSONSuccessOmitCursorsAlive) {
    StatusWith<KillCursorsResponse> result = KillCursorsResponse::parseFromBSON(
        BSON("cursorsKilled" << BSON_ARRAY(CursorId(123)) << "cursorsNotFound"
                             << BSON_ARRAY(CursorId(456) << CursorId(6))
                             << "cursorsUnknown"
                             << BSON_ARRAY(CursorId(789))
                             << "ok"
                             << 1.0));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsResponseTest, parseFromBSONCommandNotOk) {
    StatusWith<KillCursorsResponse> result =
        KillCursorsResponse::parseFromBSON(BSON("ok" << 0.0 << "code" << 123 << "errmsg"
                                                     << "does not work"));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), 123);
    ASSERT_EQ(result.getStatus().reason(), "does not work");
}

TEST(KillCursorsResponseTest, parseFromBSONFieldNotArray) {
    StatusWith<KillCursorsResponse> result = KillCursorsResponse::parseFromBSON(
        BSON("cursorsKilled" << BSON_ARRAY(CursorId(123)) << "cursorsNotFound"
                             << "foobar"
                             << "cursorsAlive"
                             << BSON_ARRAY(CursorId(7) << CursorId(8) << CursorId(9))
                             << "ok"
                             << 1.0));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsResponseTest, parseFromBSONArrayContainsInvalidElement) {
    StatusWith<KillCursorsResponse> result = KillCursorsResponse::parseFromBSON(
        BSON("cursorsKilled" << BSON_ARRAY(CursorId(123)) << "cursorsNotFound"
                             << BSON_ARRAY(CursorId(456) << CursorId(6))
                             << "cursorsAlive"
                             << BSON_ARRAY(CursorId(7) << "foobar" << CursorId(9))
                             << "ok"
                             << 1.0));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(KillCursorsResponseTest, toBSON) {
    std::vector<CursorId> killed = {CursorId(123)};
    std::vector<CursorId> notFound = {CursorId(456), CursorId(6)};
    std::vector<CursorId> alive = {CursorId(7), CursorId(8), CursorId(9)};
    std::vector<CursorId> unknown;
    KillCursorsResponse response(killed, notFound, alive, unknown);
    BSONObj responseObj = response.toBSON();
    BSONObj expectedResponse =
        BSON("cursorsKilled" << BSON_ARRAY(CursorId(123)) << "cursorsNotFound"
                             << BSON_ARRAY(CursorId(456) << CursorId(6))
                             << "cursorsAlive"
                             << BSON_ARRAY(CursorId(7) << CursorId(8) << CursorId(9))
                             << "cursorsUnknown"
                             << BSONArray()
                             << "ok"
                             << 1.0);
    ASSERT_EQ(responseObj, expectedResponse);
}

}  // namespace

}  // namespace mongo
