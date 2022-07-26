/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <algorithm>

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/count_command_as_aggregation_command.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

static const NamespaceString testns("TestDB.TestColl");

const IDLParserContext ctxt("count");

TEST(CountCommandTest, ParserDealsWithMissingFieldsCorrectly) {
    auto commandObj = BSON("count"
                           << "TestColl"
                           << "$db"
                           << "TestDB"
                           << "query" << BSON("a" << BSON("$lte" << 10)));
    auto countCmd = CountCommandRequest::parse(ctxt, commandObj);

    ASSERT_BSONOBJ_EQ(countCmd.getQuery(), fromjson("{ a : { '$lte' : 10 } }"));

    ASSERT(countCmd.getHint().isEmpty());
    ASSERT_FALSE(countCmd.getLimit());
    ASSERT_FALSE(countCmd.getSkip());
    ASSERT_FALSE(countCmd.getCollation());
    ASSERT_FALSE(countCmd.getReadConcern());
    ASSERT_FALSE(countCmd.getQueryOptions());
}

TEST(CountCommandTest, ParserParsesCommandWithAllFieldsCorrectly) {
    auto commandObj = BSON("count"
                           << "TestColl"
                           << "$db"
                           << "TestDB"
                           << "query" << BSON("a" << BSON("$gte" << 11)) << "limit" << 100 << "skip"
                           << 1000 << "hint" << BSON("b" << 5) << "collation"
                           << BSON("locale"
                                   << "en_US")
                           << "readConcern"
                           << BSON("level"
                                   << "linearizable")
                           << "$queryOptions"
                           << BSON("$readPreference"
                                   << "secondary")
                           << "comment"
                           << "aComment"
                           << "maxTimeMS" << 10000);
    const auto countCmd = CountCommandRequest::parse(ctxt, commandObj);

    ASSERT_BSONOBJ_EQ(countCmd.getQuery(), fromjson("{ a : { '$gte' : 11 } }"));
    ASSERT_EQ(countCmd.getLimit().get(), 100);
    ASSERT_EQ(countCmd.getSkip().get(), 1000);
    ASSERT_EQ(countCmd.getMaxTimeMS().get(), 10000u);
    ASSERT_BSONOBJ_EQ(countCmd.getHint(), fromjson("{ b : 5 }"));
    ASSERT_BSONOBJ_EQ(countCmd.getCollation().get(), fromjson("{ locale : 'en_US' }"));
    ASSERT_BSONOBJ_EQ(countCmd.getReadConcern().get(), fromjson("{ level: 'linearizable' }"));
    ASSERT_BSONOBJ_EQ(countCmd.getQueryOptions().get(),
                      fromjson("{ $readPreference: 'secondary' }"));
}

TEST(CountCommandTest, ParsingNegativeLimitGivesPositiveLimit) {
    auto commandObj = BSON("count"
                           << "TestColl"
                           << "$db"
                           << "TestDB"
                           << "limit" << -100);
    const auto countCmd = CountCommandRequest::parse(ctxt, commandObj);

    ASSERT_EQ(countCmd.getLimit().get(), 100);
}

TEST(CountCommandTest, LimitCannotBeMinLong) {
    auto commandObj = BSON("count"
                           << "TestColl"
                           << "$db"
                           << "TestDB"
                           << "query" << BSON("a" << BSON("$gte" << 11)) << "limit"
                           << std::numeric_limits<long long>::min());

    ASSERT_THROWS_CODE(
        CountCommandRequest::parse(ctxt, commandObj), AssertionException, ErrorCodes::BadValue);
}

TEST(CountCommandTest, FailLargerThan32BitMaxTimeMS) {
    const long long kLargerThan32BitInt =
        static_cast<long long>(std::numeric_limits<int>::max()) + 1;
    auto commandObj = BSON("count"
                           << "TestColl"
                           << "$db"
                           << "TestDB"
                           << "maxTimeMS" << kLargerThan32BitInt);

    ASSERT_THROWS_CODE(
        CountCommandRequest::parse(ctxt, commandObj), AssertionException, ErrorCodes::BadValue);
}

TEST(CountCommandTest, FailParseBadSkipValue) {
    ASSERT_THROWS_CODE(CountCommandRequest::parse(ctxt,
                                                  BSON("count"
                                                       << "TestColl"
                                                       << "$db"
                                                       << "TestDB"
                                                       << "query" << BSON("a" << BSON("$gte" << 11))
                                                       << "skip" << -1000)),
                       AssertionException,
                       51024);
}

TEST(CountCommandTest, FailParseBadCollationType) {
    ASSERT_THROWS_CODE(
        CountCommandRequest::parse(ctxt,
                                   BSON("count"
                                        << "TestColl"
                                        << "$db"
                                        << "TestDB"
                                        << "query" << BSON("a" << BSON("$gte" << 11)) << "collation"
                                        << "en_US")),
        AssertionException,
        ErrorCodes::TypeMismatch);
}

TEST(CountCommandTest, FailParseUnknownField) {
    ASSERT_THROWS_CODE(CountCommandRequest::parse(ctxt,
                                                  BSON("count"
                                                       << "TestColl"
                                                       << "$db"
                                                       << "TestDB"
                                                       << "foo"
                                                       << "bar")),
                       AssertionException,
                       40415);
}

TEST(CountCommandTest, ConvertToAggregationWithHint) {
    auto commandObj = BSON("count"
                           << "TestColl"
                           << "$db"
                           << "TestDB"
                           << "hint" << BSON("x" << 1));
    auto countCmd = CountCommandRequest::parse(ctxt, commandObj);
    auto agg = uassertStatusOK(countCommandAsAggregationCommand(countCmd, testns));
    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg).body;

    auto ar = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(testns, cmdObj));
    ASSERT_BSONOBJ_EQ(ar.getHint().value_or(BSONObj()), BSON("x" << 1));

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithQueryAndFilterAndLimit) {
    auto commandObj = BSON("count"
                           << "TestColl"
                           << "$db"
                           << "TestDB"
                           << "limit" << 200 << "skip" << 300 << "query" << BSON("x" << 7));
    auto countCmd = CountCommandRequest::parse(ctxt, commandObj);
    auto agg = uassertStatusOK(countCommandAsAggregationCommand(countCmd, testns));
    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg).body;

    auto ar = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(testns, cmdObj));
    ASSERT_EQ(ar.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSONObj());

    std::vector<BSONObj> expectedPipeline{BSON("$match" << BSON("x" << 7)),
                                          BSON("$skip" << 300),
                                          BSON("$limit" << 200),
                                          BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithMaxTimeMS) {
    auto countCmd = CountCommandRequest::parse(ctxt,
                                               BSON("count"
                                                    << "TestColl"
                                                    << "maxTimeMS" << 100 << "$db"
                                                    << "TestDB"));
    auto agg = uassertStatusOK(countCommandAsAggregationCommand(countCmd, testns));
    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg).body;

    auto ar = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(testns, cmdObj));
    ASSERT_EQ(ar.getMaxTimeMS().value_or(0), 100u);

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithQueryOptions) {
    auto countCmd = CountCommandRequest::parse(ctxt,
                                               BSON("count"
                                                    << "TestColl"
                                                    << "$db"
                                                    << "TestDB"));
    countCmd.setQueryOptions(BSON("readPreference"
                                  << "secondary"));
    auto agg = uassertStatusOK(countCommandAsAggregationCommand(countCmd, testns));
    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg).body;

    auto ar = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(testns, cmdObj));
    ASSERT_BSONOBJ_EQ(ar.getUnwrappedReadPref().value_or(BSONObj()),
                      BSON("readPreference"
                           << "secondary"));

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithReadConcern) {
    auto countCmd = CountCommandRequest::parse(ctxt,
                                               BSON("count"
                                                    << "TestColl"
                                                    << "$db"
                                                    << "TestDB"));
    countCmd.setReadConcern(BSON("level"
                                 << "linearizable"));
    auto agg = uassertStatusOK(countCommandAsAggregationCommand(countCmd, testns));
    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg).body;

    auto ar = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(testns, cmdObj));
    ASSERT_BSONOBJ_EQ(ar.getReadConcern().value_or(BSONObj()),
                      BSON("level"
                           << "linearizable"));

    std::vector<BSONObj> expectedPipeline{BSON("$count"
                                               << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

}  // namespace
}  // namespace mongo
