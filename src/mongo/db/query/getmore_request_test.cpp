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

#include "mongo/db/cursor_id.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/optime.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

GetMoreCommandRequest createGetMoreCommandRequest(
    std::string collection,
    std::int64_t cursorId,
    boost::optional<std::int64_t> sizeOfBatch = boost::none,
    boost::optional<std::int64_t> awaitDataTimeout = boost::none,
    boost::optional<std::int64_t> term = boost::none,
    boost::optional<repl::OpTime> lastKnownCommittedOpTime = boost::none) {
    GetMoreCommandRequest request(cursorId, collection);
    request.setBatchSize(sizeOfBatch);
    request.setMaxTimeMS(awaitDataTimeout);
    request.setTerm(term);
    request.setLastKnownCommittedOpTime(lastKnownCommittedOpTime);
    return request;
}

TEST(GetMoreRequestTest, toBSONMissingOptionalFields) {
    GetMoreCommandRequest request = createGetMoreCommandRequest("testcoll", 123);
    BSONObj requestObj = request.toBSON({});
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll");
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONNoMissingFields) {
    GetMoreCommandRequest request =
        createGetMoreCommandRequest("testcoll", 123, 99, 789, 1, repl::OpTime(Timestamp(0, 10), 2));
    BSONObj requestObj = request.toBSON({});
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "batchSize" << 99 << "maxTimeMS" << 789 << "term"
                                             << 1 << "lastKnownCommittedOpTime"
                                             << BSON("ts" << Timestamp(0, 10) << "t" << 2LL));
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
}

}  // namespace
