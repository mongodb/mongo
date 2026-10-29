// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
