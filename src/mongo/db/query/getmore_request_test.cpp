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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

std::unique_ptr<GetMoreCommandRequest> parseFromBSON(const BSONObj& cmdObj) {
    return std::make_unique<GetMoreCommandRequest>(
        GetMoreCommandRequest::parse(cmdObj, IDLParserContext("GetMoreCommandRequest")));
}

TEST(GetMoreRequestTest, ShouldParseAllKnownOptions) {
    repl::OpTime optime{Timestamp{0, 100}, 2};
    BSONObj inputBson = BSON("getMore" << CursorId(123) << "collection"
                                       << "testcoll"
                                       << "$db"
                                       << "testdb"
                                       << "batchSize" << 99 << "maxTimeMS" << 789 << "term" << 1LL
                                       << "lastKnownCommittedOpTime" << optime.toBSON()
                                       << "includeQueryStatsMetrics" << true);

    auto request = parseFromBSON(inputBson);

    ASSERT_TRUE(request->getBatchSize());
    ASSERT_TRUE(request->getMaxTimeMS());
    ASSERT_TRUE(request->getTerm());
    ASSERT_TRUE(request->getLastKnownCommittedOpTime());

    ASSERT_EQ(*request->getBatchSize(), 99);
    ASSERT_EQ(*request->getMaxTimeMS(), 789);
    ASSERT_EQ(*request->getTerm(), 1LL);
    ASSERT_EQ(*request->getLastKnownCommittedOpTime(), optime);
    ASSERT_TRUE(request->getIncludeQueryStatsMetrics());
}

TEST(GetMoreRequestTest, toBSONMissingOptionalFields) {
    GetMoreCommandRequest request = createGetMoreCommandRequest("testcoll", 123);
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll");
    ASSERT_BSONOBJ_EQ(requestObj, expectedRequest);
}

TEST(GetMoreRequestTest, toBSONNoMissingFields) {
    GetMoreCommandRequest request =
        createGetMoreCommandRequest("testcoll", 123, 99, 789, 1, repl::OpTime(Timestamp(0, 10), 2));
    BSONObj requestObj = request.toBSON();
    BSONObj expectedRequest = BSON("getMore" << CursorId(123) << "collection"
                                             << "testcoll"
                                             << "batchSize" << 99 << "maxTimeMS" << 789 << "term"
                                             << 1 << "lastKnownCommittedOpTime"
                                             << BSON("ts" << Timestamp(0, 10) << "t" << 2LL));
    ASSERT_BSONOBJ_EQ_UNORDERED(requestObj, expectedRequest);
}

}  // namespace
