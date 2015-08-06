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

#include "mongo/db/query/getmore_response.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(GetMoreResponseTest, parseFromBSONFirstBatch) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1));
    ASSERT_OK(result.getStatus());

    GetMoreResponse response = result.getValue();
    ASSERT_EQ(response.cursorId, CursorId(123));
    ASSERT_EQ(response.nss.ns(), "db.coll");
    ASSERT_EQ(response.batch.size(), 2U);
    ASSERT_EQ(response.batch[0], BSON("_id" << 1));
    ASSERT_EQ(response.batch[1], BSON("_id" << 2));
}

TEST(GetMoreResponseTest, parseFromBSONNextBatch) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1));
    ASSERT_OK(result.getStatus());

    GetMoreResponse response = result.getValue();
    ASSERT_EQ(response.cursorId, CursorId(123));
    ASSERT_EQ(response.nss.ns(), "db.coll");
    ASSERT_EQ(response.batch.size(), 2U);
    ASSERT_EQ(response.batch[0], BSON("_id" << 1));
    ASSERT_EQ(response.batch[1], BSON("_id" << 2));
}

TEST(GetMoreResponseTest, parseFromBSONCursorIdZero) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(0) << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1));
    ASSERT_OK(result.getStatus());

    GetMoreResponse response = result.getValue();
    ASSERT_EQ(response.cursorId, CursorId(0));
    ASSERT_EQ(response.nss.ns(), "db.coll");
    ASSERT_EQ(response.batch.size(), 2U);
    ASSERT_EQ(response.batch[0], BSON("_id" << 1));
    ASSERT_EQ(response.batch[1], BSON("_id" << 2));
}

TEST(GetMoreResponseTest, parseFromBSONEmptyBatch) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(
        BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSONArrayBuilder().arr()) << "ok" << 1));
    ASSERT_OK(result.getStatus());

    GetMoreResponse response = result.getValue();
    ASSERT_EQ(response.cursorId, CursorId(123));
    ASSERT_EQ(response.nss.ns(), "db.coll");
    ASSERT_EQ(response.batch.size(), 0U);
}

TEST(GetMoreResponseTest, parseFromBSONMissingCursorField) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(BSON("ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONCursorFieldWrongType) {
    StatusWith<GetMoreResponse> result =
        GetMoreResponse::parseFromBSON(BSON("cursor" << 3 << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONNsFieldMissing) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "firstBatch"
                              << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2))) << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONNsFieldWrongType) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns" << 456 << "firstBatch"
                              << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2))) << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONIdFieldMissing) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(
        BSON("cursor" << BSON("ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                      << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONIdFieldWrongType) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(
        BSON("cursor" << BSON("id"
                              << "123"
                              << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                      << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONBatchFieldMissing) {
    StatusWith<GetMoreResponse> result =
        GetMoreResponse::parseFromBSON(BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                                                  << "db.coll") << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONFirstBatchFieldWrongType) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(
        BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON("_id" << 1)) << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONNextBatchFieldWrongType) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(
        BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSON("_id" << 1)) << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONOkFieldMissing) {
    StatusWith<GetMoreResponse> result = GetMoreResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(GetMoreResponseTest, parseFromBSONHandleErrorResponse) {
    StatusWith<GetMoreResponse> result =
        GetMoreResponse::parseFromBSON(BSON("ok" << 0 << "code" << 123 << "errmsg"
                                                 << "does not work"));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), 123);
    ASSERT_EQ(result.getStatus().reason(), "does not work");
}

TEST(GetMoreResponseTest, toBSON) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    GetMoreResponse response(NamespaceString("testdb.testcoll"), CursorId(123), batch);
    BSONObj responseObj = response.toBSON();
    BSONObj expectedResponse = BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "testdb.testcoll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1.0);
    ASSERT_EQ(responseObj, expectedResponse);
}

TEST(GetMoreResponseTest, toBSONWithBuilder) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    GetMoreResponse response(NamespaceString("testdb.testcoll"), CursorId(123), batch);

    BSONObjBuilder builder;
    response.toBSON(&builder);
    BSONObj responseObj = builder.obj();

    BSONObj expectedResponse = BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "testdb.testcoll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1.0);
    ASSERT_EQ(responseObj, expectedResponse);
}

}  // namespace

}  // namespace mongo
