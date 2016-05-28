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

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/repl/optime.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(GetMoreRequestTest, parseFromBSONEmptyCommandObject) {
    StatusWith<GetMoreRequest> result = GetMoreRequest::parseFromBSON("db", BSONObj());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONCursorIdNotNumeric) {
    StatusWith<GetMoreRequest> result = GetMoreRequest::parseFromBSON("db",
                                                                      BSON("getMore"
                                                                           << "not a number"
                                                                           << "collection"
                                                                           << "coll"));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONCursorIdNotLongLong) {
    StatusWith<GetMoreRequest> result = GetMoreRequest::parseFromBSON("db",
                                                                      BSON("getMore"
                                                                           << "not a number"
                                                                           << "collection"
                                                                           << 123));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONMissingCollection) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db", BSON("getMore" << CursorId(123)));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONCollectionNotString) {
    StatusWith<GetMoreRequest> result = GetMoreRequest::parseFromBSON(
        "db", BSON("getMore" << CursorId(123) << "collection" << 456));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONBatchSizeNotInteger) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "batchSize"
                                                     << "not a number"));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONInvalidCursorId) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(0) << "collection"
                                                     << "coll"));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONNegativeCursorId) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(-123) << "collection"
                                                     << "coll"));
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("db.coll", result.getValue().nss.toString());
    ASSERT_EQUALS(CursorId(-123), result.getValue().cursorid);
    ASSERT_FALSE(result.getValue().batchSize);
}

TEST(GetMoreRequestTest, parseFromBSONUnrecognizedFieldName) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "unknown_field"
                                                     << 1));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONInvalidBatchSize) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "batchSize"
                                                     << -1));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONInvalidBatchSizeOfZero) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "batchSize"
                                                     << 0));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(GetMoreRequestTest, parseFromBSONNoBatchSize) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"));
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("db.coll", result.getValue().nss.toString());
    ASSERT_EQUALS(CursorId(123), result.getValue().cursorid);
    ASSERT_FALSE(result.getValue().batchSize);
}

TEST(GetMoreRequestTest, parseFromBSONBatchSizeProvided) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "batchSize"
                                                     << 200));
    ASSERT_EQUALS("db.coll", result.getValue().nss.toString());
    ASSERT_EQUALS(CursorId(123), result.getValue().cursorid);
    ASSERT(result.getValue().batchSize);
    ASSERT_EQUALS(200, *result.getValue().batchSize);
}

TEST(GetMoreRequestTest, parseFromBSONIgnoreDollarPrefixedFields) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "$foo"
                                                     << "bar"));
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("db.coll", result.getValue().nss.toString());
    ASSERT_EQUALS(CursorId(123), result.getValue().cursorid);
}

TEST(GetMoreRequestTest, parseFromBSONHasMaxTimeMS) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "maxTimeMS"
                                                     << 100));
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("db.coll", result.getValue().nss.toString());
    ASSERT(result.getValue().awaitDataTimeout);
    ASSERT_EQUALS(100, durationCount<Milliseconds>(*result.getValue().awaitDataTimeout));
    ASSERT_EQUALS(CursorId(123), result.getValue().cursorid);
}

TEST(GetMoreRequestTest, parseFromBSONHasMaxTimeMSOfZero) {
    StatusWith<GetMoreRequest> result =
        GetMoreRequest::parseFromBSON("db",
                                      BSON("getMore" << CursorId(123) << "collection"
                                                     << "coll"
                                                     << "maxTimeMS"
                                                     << 0));
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("db.coll", result.getValue().nss.toString());
    ASSERT_EQUALS(CursorId(123), result.getValue().cursorid);

    // Max time of 0 means the same thing as no max time.
    ASSERT(!result.getValue().awaitDataTimeout);
}

TEST(GetMoreRequestTest, toBSONHasBatchSize) {
    GetMoreRequest request(
        NamespaceString("testdb.testcoll"), 123, 99, boost::none, boost::none, boost::none);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "batchSize"
                                             << 99);
    ASSERT_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONMissingMatchSize) {
    GetMoreRequest request(NamespaceString("testdb.testcoll"),
                           123,
                           boost::none,
                           boost::none,
                           boost::none,
                           boost::none);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll");
    ASSERT_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONHasTerm) {
    GetMoreRequest request(
        NamespaceString("testdb.testcoll"), 123, 99, boost::none, 1, boost::none);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "batchSize"
                                             << 99
                                             << "term"
                                             << 1);
    ASSERT_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONHasCommitLevel) {
    GetMoreRequest request(NamespaceString("testdb.testcoll"),
                           123,
                           99,
                           boost::none,
                           1,
                           repl::OpTime(Timestamp(0, 10), 2));
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "batchSize"
                                             << 99
                                             << "term"
                                             << 1
                                             << "lastKnownCommittedOpTime"
                                             << BSON("ts" << Timestamp(0, 10) << "t" << 2LL));
    ASSERT_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONHasMaxTimeMS) {
    GetMoreRequest request(NamespaceString("testdb.testcoll"),
                           123,
                           boost::none,
                           Milliseconds(789),
                           boost::none,
                           boost::none);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "maxTimeMS"
                                             << 789);
    ASSERT_EQ(requestObj, expectedRequest);
}

}  // namespace
