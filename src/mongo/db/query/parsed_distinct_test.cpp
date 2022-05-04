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

#include <algorithm>

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static const NamespaceString testns("testdb.testcoll");
static const bool isExplain = true;

TEST(ParsedDistinctTest, ConvertToAggregationNoQuery) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(opCtx,
                                    testns,
                                    fromjson("{distinct: 'testcoll', key: 'x', $db: 'testdb'}"),
                                    ExtensionsCallbackNoop(),
                                    !isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
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

TEST(ParsedDistinctTest, ConvertToAggregationDottedPathNoQuery) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(opCtx,
                                    testns,
                                    fromjson("{distinct: 'testcoll', key: 'x.y.z', $db: 'testdb'}"),
                                    ExtensionsCallbackNoop(),
                                    !isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
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

TEST(ParsedDistinctTest, ConvertToAggregationWithAllOptions) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(opCtx,
                                    testns,
                                    BSON("distinct"
                                         << "testcoll"
                                         << "key"
                                         << "x"
                                         << "collation"
                                         << BSON("locale"
                                                 << "en_US")
                                         << "readConcern"
                                         << BSON("level"
                                                 << "linearizable")
                                         << "$queryOptions"
                                         << BSON("readPreference"
                                                 << "secondary")
                                         << "maxTimeMS" << 100 << "$db"
                                         << "testdb"),
                                    ExtensionsCallbackNoop(),
                                    !isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
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

TEST(ParsedDistinctTest, ConvertToAggregationWithQuery) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(
        opCtx,
        testns,
        fromjson("{distinct: 'testcoll', key: 'y', query: {z: 7}, $db: 'testdb'}"),
        ExtensionsCallbackNoop(),
        !isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
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

TEST(ParsedDistinctTest, ExplainNotIncludedWhenConvertingToAggregationCommand) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    auto pd = ParsedDistinct::parse(opCtx,
                                    testns,
                                    fromjson("{distinct: 'testcoll', key: 'x', $db: 'testdb'}"),
                                    ExtensionsCallbackNoop(),
                                    isExplain);
    ASSERT_OK(pd.getStatus());

    auto agg = pd.getValue().asAggregationCommand();
    ASSERT_OK(agg);

    ASSERT_FALSE(agg.getValue().hasField("explain"));

    auto cmdObj = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
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

TEST(ParsedDistinctTest, FailsToParseDistinctWithUnknownFields) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    BSONObj cmdObj = fromjson(R"({
        distinct: 'testcoll',
        key: "a",
        $db: 'testdb',
        unknown: 1
    })");

    auto parsedDistinct =
        ParsedDistinct::parse(opCtx, testns, cmdObj, ExtensionsCallbackNoop(), false);
    ASSERT_EQ(parsedDistinct.getStatus().code(), 40415);
}

TEST(QueryRequestTest, FailsToParseDistinctWithMissingKey) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        $db: 'testdb'
    })");

    auto parsedDistinct =
        ParsedDistinct::parse(opCtx, testns, cmdObj, ExtensionsCallbackNoop(), false);
    ASSERT_EQ(parsedDistinct.getStatus().code(), 40414);
}

TEST(QueryRequestTest, FailsToParseDistinctWithInvalidKeyType) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        key: {a: 1},
        $db: 'test'
    })");

    auto parsedDistinct =
        ParsedDistinct::parse(opCtx, testns, cmdObj, ExtensionsCallbackNoop(), false);
    ASSERT_EQ(parsedDistinct.getStatus().code(), ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, InvalidGenericCommandArgumentsAreIgnored) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();

    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        key: 'a',
        readConcern: {level: 'invalid'},
        $db: 'test'
    })");

    auto parsedDistinct =
        ParsedDistinct::parse(opCtx, testns, cmdObj, ExtensionsCallbackNoop(), false);
    ASSERT_OK(parsedDistinct.getStatus());
}

}  // namespace
}  // namespace mongo
