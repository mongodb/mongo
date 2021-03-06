/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/repl/optime.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(GetMoreRequestTest, toBSONHasBatchSize) {
    GetMoreRequest request(
        NamespaceString("testdb.testcoll"), 123, 99, boost::none, boost::none, boost::none);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "batchSize" << 99);
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
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
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONHasTerm) {
    GetMoreRequest request(
        NamespaceString("testdb.testcoll"), 123, 99, boost::none, 1, boost::none);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "batchSize" << 99 << "term" << 1);
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONHasCommitLevel) {
    GetMoreRequest request(NamespaceString("testdb.testcoll"),
                           123,
                           99,
                           boost::none,
                           1,
                           repl::OpTime(Timestamp(0, 10), 2));
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest =
        BSON("getMore" << CursorId(123) << "collection"
                       << "testcoll"
                       << "batchSize" << 99 << "term" << 1 << "lastKnownCommittedOpTime"
                       << BSON("ts" << Timestamp(0, 10) << "t" << 2LL));
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
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
                                             << "maxTimeMS" << 789);
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
}

}  // namespace
