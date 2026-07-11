// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <limits>
#include <vector>


namespace mongo {
namespace {

static const NamespaceString testns =
    NamespaceString::createNamespaceString_forTest("TestDB.TestColl");

const IDLParserContext ctxt("count");

TEST(CountCommandTest, ParserDealsWithMissingFieldsCorrectly) {
    auto commandObj = BSON("count" << "TestColl"
                                   << "$db"
                                   << "TestDB"
                                   << "query" << BSON("a" << BSON("$lte" << 10)));
    auto countCmd = CountCommandRequest::parse(commandObj, ctxt);

    ASSERT_BSONOBJ_EQ(countCmd.getQuery(), fromjson("{ a : { '$lte' : 10 } }"));

    ASSERT(countCmd.getHint().isEmpty());
    ASSERT_FALSE(countCmd.getLimit());
    ASSERT_FALSE(countCmd.getSkip());
    ASSERT_FALSE(countCmd.getCollation());
    ASSERT_FALSE(countCmd.getReadConcern());
    ASSERT_FALSE(countCmd.getUnwrappedReadPref());
}

TEST(CountCommandTest, ParserParsesCommandWithAllFieldsCorrectly) {
    auto commandObj =
        BSON("count" << "TestColl"
                     << "$db"
                     << "TestDB"
                     << "query" << BSON("a" << BSON("$gte" << 11)) << "limit" << 100 << "skip"
                     << 1000 << "hint" << BSON("b" << 5) << "collation" << BSON("locale" << "en_US")
                     << "readConcern" << BSON("level" << "linearizable") << "$queryOptions"
                     << BSON("$readPreference" << "secondary") << "comment"
                     << "aComment"
                     << "maxTimeMS" << 10000);
    const auto countCmd = CountCommandRequest::parse(commandObj, ctxt);

    ASSERT_BSONOBJ_EQ(countCmd.getQuery(), fromjson("{ a : { '$gte' : 11 } }"));
    ASSERT_EQ(countCmd.getLimit().value(), 100);
    ASSERT_EQ(countCmd.getSkip().value(), 1000);
    ASSERT_EQ(countCmd.getMaxTimeMS().value(), 10000u);
    ASSERT_BSONOBJ_EQ(countCmd.getHint(), fromjson("{ b : 5 }"));
    ASSERT_BSONOBJ_EQ(countCmd.getCollation().value(), fromjson("{ locale : 'en_US' }"));
    ASSERT_BSONOBJ_EQ(countCmd.getReadConcern()->toBSONInner(),
                      fromjson("{ level: 'linearizable' }"));
    ASSERT_BSONOBJ_EQ(countCmd.getUnwrappedReadPref().value(),
                      fromjson("{ $readPreference: 'secondary' }"));
}

TEST(CountCommandTest, ParsingNegativeLimitGivesPositiveLimit) {
    auto commandObj = BSON("count" << "TestColl"
                                   << "$db"
                                   << "TestDB"
                                   << "limit" << -100);
    const auto countCmd = CountCommandRequest::parse(commandObj, ctxt);

    ASSERT_EQ(countCmd.getLimit().value(), 100);
}

TEST(CountCommandTest, LimitCannotBeMinLong) {
    auto commandObj = BSON("count" << "TestColl"
                                   << "$db"
                                   << "TestDB"
                                   << "query" << BSON("a" << BSON("$gte" << 11)) << "limit"
                                   << std::numeric_limits<long long>::min());

    ASSERT_THROWS_CODE(
        CountCommandRequest::parse(commandObj, ctxt), AssertionException, ErrorCodes::BadValue);
}

TEST(CountCommandTest, FailParseBadSkipValue) {
    ASSERT_THROWS_CODE(
        CountCommandRequest::parse(BSON("count" << "TestColl"
                                                << "$db"
                                                << "TestDB"
                                                << "query" << BSON("a" << BSON("$gte" << 11))
                                                << "skip" << -1000),
                                   ctxt),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST(CountCommandTest, FailParseBadCollationType) {
    ASSERT_THROWS_CODE(CountCommandRequest::parse(
                           BSON("count" << "TestColl"
                                        << "$db"
                                        << "TestDB"
                                        << "query" << BSON("a" << BSON("$gte" << 11)) << "collation"
                                        << "en_US"),
                           ctxt),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(CountCommandTest, FailParseUnknownField) {
    ASSERT_THROWS_CODE(CountCommandRequest::parse(BSON("count" << "TestColl"
                                                               << "$db"
                                                               << "TestDB"
                                                               << "foo"
                                                               << "bar"),
                                                  ctxt),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST(CountCommandTest, ConvertToAggregationWithHint) {
    auto commandObj = BSON("count" << "TestColl"
                                   << "$db"
                                   << "TestDB"
                                   << "hint" << BSON("x" << 1));
    auto countCmd = CountCommandRequest::parse(commandObj, ctxt);
    auto ar = query_request_conversion::asAggregateCommandRequest(countCmd);
    ASSERT_BSONOBJ_EQ(ar.getHint().value_or(BSONObj()), BSON("x" << 1));

    std::vector<BSONObj> expectedPipeline{BSON("$count" << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithQueryAndFilterAndLimit) {
    auto commandObj = BSON("count" << "TestColl"
                                   << "$db"
                                   << "TestDB"
                                   << "limit" << 200 << "skip" << 300 << "query" << BSON("x" << 7));
    auto countCmd = CountCommandRequest::parse(commandObj, ctxt);
    auto ar = query_request_conversion::asAggregateCommandRequest(countCmd);
    ASSERT_EQ(ar.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSONObj());

    std::vector<BSONObj> expectedPipeline{BSON("$match" << BSON("x" << 7)),
                                          BSON("$skip" << 300),
                                          BSON("$limit" << 200),
                                          BSON("$count" << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithMaxTimeMS) {
    auto countCmd = CountCommandRequest::parse(BSON("count" << "TestColl"
                                                            << "maxTimeMS" << 100 << "$db"
                                                            << "TestDB"),
                                               ctxt);
    auto ar = query_request_conversion::asAggregateCommandRequest(countCmd);
    ASSERT_EQ(ar.getMaxTimeMS().value_or(0), 100u);

    std::vector<BSONObj> expectedPipeline{BSON("$count" << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithQueryOptions) {
    auto countCmd = CountCommandRequest::parse(BSON("count" << "TestColl"
                                                            << "$db"
                                                            << "TestDB"),
                                               ctxt);
    countCmd.setUnwrappedReadPref(BSON("readPreference" << "secondary"));
    auto ar = query_request_conversion::asAggregateCommandRequest(countCmd);
    ASSERT_BSONOBJ_EQ(ar.getUnwrappedReadPref().value_or(BSONObj()),
                      BSON("readPreference" << "secondary"));

    std::vector<BSONObj> expectedPipeline{BSON("$count" << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(CountCommandTest, ConvertToAggregationWithReadConcern) {
    auto countCmd = CountCommandRequest::parse(BSON("count" << "TestColl"
                                                            << "$db"
                                                            << "TestDB"),
                                               ctxt);
    countCmd.setReadConcern(repl::ReadConcernArgs::kLinearizable);
    auto ar = query_request_conversion::asAggregateCommandRequest(countCmd);
    ASSERT_TRUE(ar.getReadConcern().has_value());
    ASSERT_BSONOBJ_EQ(ar.getReadConcern()->toBSONInner(),
                      repl::ReadConcernArgs::kLinearizable.toBSONInner());

    std::vector<BSONObj> expectedPipeline{BSON("$count" << "count")};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      ar.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

}  // namespace
}  // namespace mongo
