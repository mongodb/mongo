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

#include <algorithm>
#include <boost/cstdint.hpp>
#include <cstdint>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

static const NamespaceString testns =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");
static const bool isExplain = true;

class CanonicalDistinctTest : public mongo::unittest::Test {
public:
    CanonicalDistinctTest() {
        uniqueTxn = serviceContext.makeOperationContext();
        opCtx = uniqueTxn.get();
        expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, testns);
        expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
        expCtx->explain = boost::none;
    }

private:
    QueryTestServiceContext serviceContext;
    ServiceContext::UniqueOperationContext uniqueTxn;
    OperationContext* opCtx;

protected:
    boost::intrusive_ptr<ExpressionContext> expCtx;
};

std::unique_ptr<ParsedDistinctCommand> bsonToParsedDistinct(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& cmd) {
    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        IDLParserContext("distinctCommandRequest",
                         false /* apiStrict */,
                         auth::ValidatedTenancyScope::get(expCtx->opCtx),
                         boost::none),
        cmd));
    return parsed_distinct_command::parse(expCtx,
                                          cmd,
                                          std::move(distinctCommand),
                                          ExtensionsCallbackNoop(),
                                          MatchExpressionParser::kAllowAllSpecialFeatures);
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationNoQuery) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'x', $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto cd = CanonicalDistinct::parse(expCtx, std::move(pdc), nullptr);

    auto agg = cd.asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequestBuilder::create(
                      auth::ValidatedTenancyScope::kNotRequired, testns.dbName(), agg.getValue())
                      .body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, cmdObj);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());
    ASSERT(ar.getValue().getReadConcern().value_or(BSONObj()).isEmpty());
    ASSERT(ar.getValue().getUnwrappedReadPref().value_or(BSONObj()).isEmpty());
    ASSERT_EQUALS(ar.getValue().getMaxTimeMS().value_or(0), 0u);

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON(
                 "newRoot" << BSON("_internalUnwoundArray" << BSON("$_internalFindAllValuesAtPath"
                                                                   << "x")))),
        BSON("$unwind" << BSON("path"
                               << "$_internalUnwoundArray"
                               << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet"
                                            << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationDottedPathNoQuery) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'x.y.z', $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto cd = CanonicalDistinct::parse(expCtx, std::move(pdc), nullptr);

    auto agg = cd.asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequestBuilder::create(
                      auth::ValidatedTenancyScope::kNotRequired, testns.dbName(), agg.getValue())
                      .body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, cmdObj);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());
    ASSERT(ar.getValue().getReadConcern().value_or(BSONObj()).isEmpty());
    ASSERT(ar.getValue().getUnwrappedReadPref().value_or(BSONObj()).isEmpty());
    ASSERT_EQUALS(ar.getValue().getMaxTimeMS().value_or(0), 0u);

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON(
                 "newRoot" << BSON("_internalUnwoundArray" << BSON("$_internalFindAllValuesAtPath"
                                                                   << "x.y.z")))),
        BSON("$unwind" << BSON("path"
                               << "$_internalUnwoundArray"
                               << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet"
                                            << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationWithAllOptions) {
    auto rawCmd = BSON("distinct"
                       << "testcoll"
                       << "key"
                       << "x"
                       << "hint" << BSON("b" << 5) << "collation"
                       << BSON("locale"
                               << "en_US")
                       << "readConcern"
                       << BSON("level"
                               << "linearizable")
                       << "$queryOptions"
                       << BSON("readPreference"
                               << "secondary")
                       << "maxTimeMS" << 100 << "$db"
                       << "testdb");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto cd = CanonicalDistinct::parse(expCtx, std::move(pdc), nullptr);
    auto agg = cd.asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequestBuilder::create(
                      auth::ValidatedTenancyScope::kNotRequired, testns.dbName(), agg.getValue())
                      .body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, cmdObj);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()),
                      BSON("locale"
                           << "en_US"));
    ASSERT_BSONOBJ_EQ(ar.getValue().getReadConcern().value_or(BSONObj()),
                      BSON("level"
                           << "linearizable"));
    ASSERT_BSONOBJ_EQ(ar.getValue().getUnwrappedReadPref().value_or(BSONObj()),
                      BSON("readPreference"
                           << "secondary"));
    ASSERT_EQUALS(ar.getValue().getMaxTimeMS().value_or(0), 100u);
    ASSERT_BSONOBJ_EQ(ar.getValue().getHint().value_or(BSONObj()), fromjson("{ b : 5 }"));

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON(
                 "newRoot" << BSON("_internalUnwoundArray" << BSON("$_internalFindAllValuesAtPath"
                                                                   << "x")))),
        BSON("$unwind" << BSON("path"
                               << "$_internalUnwoundArray"
                               << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet"
                                            << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationWithQuery) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'y', query: {z: 7}, $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto cd = CanonicalDistinct::parse(expCtx, std::move(pdc), nullptr);

    auto agg = cd.asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequestBuilder::create(
                      auth::ValidatedTenancyScope::kNotRequired, testns.dbName(), agg.getValue())
                      .body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, cmdObj);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());
    ASSERT(ar.getValue().getReadConcern().value_or(BSONObj()).isEmpty());
    ASSERT(ar.getValue().getUnwrappedReadPref().value_or(BSONObj()).isEmpty());
    ASSERT_EQUALS(ar.getValue().getMaxTimeMS().value_or(0), 0u);

    std::vector<BSONObj> expectedPipeline{
        BSON("$match" << BSON("z" << 7)),
        BSON("$replaceRoot" << BSON(
                 "newRoot" << BSON("_internalUnwoundArray" << BSON("$_internalFindAllValuesAtPath"
                                                                   << "y")))),
        BSON("$unwind" << BSON("path"
                               << "$_internalUnwoundArray"
                               << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet"
                                            << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ExplainNotIncludedWhenConvertingToAggregationCommand) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'x', $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto cd = CanonicalDistinct::parse(expCtx, std::move(pdc), nullptr);

    auto agg = cd.asAggregationCommand();
    ASSERT_OK(agg);

    ASSERT_FALSE(agg.getValue().hasField("explain"));

    auto cmdObj = OpMsgRequestBuilder::create(
                      auth::ValidatedTenancyScope::kNotRequired, testns.dbName(), agg.getValue())
                      .body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, cmdObj);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON(
                 "newRoot" << BSON("_internalUnwoundArray" << BSON("$_internalFindAllValuesAtPath"
                                                                   << "x")))),
        BSON("$unwind" << BSON("path"
                               << "$_internalUnwoundArray"
                               << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet"
                                            << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      ar.getValue().getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, FailsToParseDistinctWithUnknownFields) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testcoll',
        key: "a",
        $db: 'testdb',
        unknown: 1
    })");

    ASSERT_THROWS_CODE(bsonToParsedDistinct(expCtx, cmdObj), DBException, 40415);
}

TEST_F(CanonicalDistinctTest, FailsToParseDistinctWithMissingKey) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        $db: 'testdb'
    })");

    ASSERT_THROWS_CODE(
        bsonToParsedDistinct(expCtx, cmdObj), DBException, ErrorCodes::IDLFailedToParse);
}

TEST_F(CanonicalDistinctTest, FailsToParseDistinctWithInvalidKeyType) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        key: {a: 1},
        $db: 'test'
    })");

    ASSERT_THROWS_CODE(bsonToParsedDistinct(expCtx, cmdObj), DBException, ErrorCodes::TypeMismatch);
}

TEST_F(CanonicalDistinctTest, InvalidGenericCommandArgumentsAreIgnored) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        key: 'a',
        readConcern: {level: 'invalid'},
        $db: 'test'
    })");

    auto pdc = bsonToParsedDistinct(expCtx, cmdObj);
    auto cd = CanonicalDistinct::parse(expCtx, std::move(pdc), nullptr);
}

}  // namespace
}  // namespace mongo
