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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

static constexpr auto kBatchSizeFieldName = "batchSize"_sd;

const Document kDefaultCursorOptionDocument{
    {kBatchSizeFieldName, aggregation_request_helper::kDefaultBatchSize}};

//
// Parsing
//

TEST(AggregationRequestTest, ShouldParseAllKnownOptions) {
    // Using oplog namespace so that validation of $_requestReshardingResumeToken succeeds.
    BSONObj inputBson = fromjson(
        "{aggregate: 'oplog.rs', pipeline: [{$match: {a: 'abc'}}], explain: false, allowDiskUse: "
        "true, fromRouter: true, "
        "needsMerge: true, bypassDocumentValidation: true, $_requestReshardingResumeToken: true, "
        "collation: {locale: 'en_US'}, cursor: {batchSize: 10}, hint: {a: 1}, maxTimeMS: 100, "
        "readConcern: {level: 'linearizable'}, $queryOptions: {$readPreference: 'nearest'}, "
        "exchange: {policy: 'roundrobin', consumers:NumberInt(2)}, isMapReduceCommand: true, $db: "
        "'local', $_isClusterQueryWithoutShardKeyCmd: true, includeQueryStatsMetrics: true}");
    auto uuid = UUID::gen();
    BSONObjBuilder uuidBob;
    uuid.appendToBuilder(&uuidBob, AggregateCommandRequest::kCollectionUUIDFieldName);
    inputBson = inputBson.addField(uuidBob.obj().firstElement());

    auto request =
        unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(inputBson));
    ASSERT_FALSE(request.getExplain());
    ASSERT_TRUE(request.getAllowDiskUse());
    ASSERT_TRUE(aggregation_request_helper::getFromRouter(request));
    ASSERT_TRUE(request.getNeedsMerge());
    ASSERT_TRUE(request.getBypassDocumentValidation().value_or(false));
    ASSERT_TRUE(request.getRequestReshardingResumeToken());
    ASSERT_TRUE(request.getIsClusterQueryWithoutShardKeyCmd());
    ASSERT_EQ(
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        10);
    ASSERT_BSONOBJ_EQ(request.getHint().value_or(BSONObj()), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(request.getCollation().value_or(BSONObj()), BSON("locale" << "en_US"));
    ASSERT_EQ(*request.getMaxTimeMS(), 100u);
    ASSERT_BSONOBJ_EQ(request.getReadConcern()->toBSONInner(), BSON("level" << "linearizable"));
    ASSERT_BSONOBJ_EQ(request.getUnwrappedReadPref().value_or(BSONObj()),
                      BSON("$readPreference" << "nearest"));
    ASSERT_TRUE(request.getExchange().has_value());
    ASSERT_TRUE(request.getIsMapReduceCommand());
    ASSERT_TRUE(request.getIncludeQueryStatsMetrics());
    ASSERT_EQ(*request.getCollectionUUID(), uuid);
}

TEST(AggregationRequestTest, ShouldParseExplicitRequestReshardingResumeTokenFalseForNonOplog) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], $_requestReshardingResumeToken: false, cursor: "
        "{}, $db: 'a'}");
    auto request =
        unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(inputBson));
    ASSERT_FALSE(request.getRequestReshardingResumeToken());
}

TEST(AggregationRequestTest, ShouldParseExplicitExplainTrue) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], explain: true, cursor: {}, $db: 'a'}");
    auto request =
        unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(inputBson));
    ASSERT(request.getExplain());
    ASSERT(request.getExplain().value());
}

TEST(AggregationRequestTest, ShouldParseExplicitExplainFalseWithCursorOption) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], explain: false, cursor: {batchSize: 10}, $db: "
        "'a'}");
    auto request =
        unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(inputBson));
    ASSERT_FALSE(request.getExplain());
    ASSERT_EQ(
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        10);
}

TEST(AggregationRequestTest, ShouldParseWithSeparateQueryPlannerExplainModeArg) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], cursor: {}, $db: 'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(
        inputBson, boost::none, ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT(request.getExplain());
    ASSERT(request.getExplain().value());
}

TEST(AggregationRequestTest, ShouldParseWithSeparateQueryPlannerExplainModeArgAndCursorOption) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], cursor: {batchSize: 10}, $db: 'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(
        inputBson, boost::none, ExplainOptions::Verbosity::kExecStats));
    ASSERT(request.getExplain());
    ASSERT(request.getExplain().value());
    ASSERT_EQ(
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        10);
}

TEST(AggregationRequestTest, ShouldParseExplainFlagWithReadConcern) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    // Non-local readConcern should not be allowed with the explain flag, but this is checked
    // elsewhere to avoid having to parse the readConcern in AggregationCommand.
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], explain: true, readConcern: {level: 'majority'}, "
        "$db: 'a'}");
    auto request =
        unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(inputBson));
    ASSERT(request.getExplain());
    ASSERT(request.getExplain().value());
    ASSERT_BSONOBJ_EQ(request.getReadConcern()->toBSONInner(), BSON("level" << "majority"));
}

//
// Serialization
//

TEST(AggregationRequestTest, ShouldOnlySerializeRequiredFieldsIfNoOptionalFieldsAreSpecified) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    AggregateCommandRequest request(nss, std::vector<mongo::BSONObj>());

    auto expectedSerialization =
        Document{{AggregateCommandRequest::kCommandName, nss.coll()},
                 {AggregateCommandRequest::kPipelineFieldName, std::vector<Value>{}},
                 {AggregateCommandRequest::kCursorFieldName, Value(kDefaultCursorOptionDocument)}};
    ASSERT_DOCUMENT_EQ(Document(request.toBSON()), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerializeOptionalValuesIfSet) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    AggregateCommandRequest request(nss, std::vector<mongo::BSONObj>());
    request.setAllowDiskUse(true);
    request.setFromRouter(true);
    request.setNeedsMerge(true);
    request.setBypassDocumentValidation(true);
    request.setRequestReshardingResumeToken(true);
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    request.setCursor(cursor);
    request.setMaxTimeMS(10u);
    const auto hintObj = BSON("a" << 1);
    request.setHint(hintObj);
    const auto collationObj = BSON("locale" << "en_US");
    request.setCollation(collationObj);
    const auto readPrefObj = BSON("$readPreference" << "nearest");
    request.setUnwrappedReadPref(readPrefObj);
    const auto readConcernObj = BSON("level" << "linearizable");
    request.setReadConcern(repl::ReadConcernArgs::kLinearizable);
    request.setIsMapReduceCommand(true);
    const auto letParamsObj = BSON("foo" << "bar");
    request.setLet(letParamsObj);
    auto uuid = UUID::gen();
    request.setCollectionUUID(uuid);
    request.setIsClusterQueryWithoutShardKeyCmd(true);

    request.setIncludeQueryStatsMetrics(true);

    auto expectedSerialization = Document{
        {AggregateCommandRequest::kCommandName, nss.coll()},
        {AggregateCommandRequest::kPipelineFieldName, std::vector<Value>{}},
        {AggregateCommandRequest::kAllowDiskUseFieldName, true},
        {AggregateCommandRequest::kCursorFieldName, Value(Document({{kBatchSizeFieldName, 10}}))},
        {AggregateCommandRequest::kBypassDocumentValidationFieldName, true},
        {AggregateCommandRequest::kCollationFieldName, collationObj},
        {AggregateCommandRequest::kHintFieldName, hintObj},
        {AggregateCommandRequest::kLetFieldName, letParamsObj},
        {AggregateCommandRequest::kNeedsMergeFieldName, true},
        {AggregateCommandRequest::kFromRouterFieldName, true},
        {AggregateCommandRequest::kRequestReshardingResumeTokenFieldName, true},
        {AggregateCommandRequest::kIsMapReduceCommandFieldName, true},
        {AggregateCommandRequest::kCollectionUUIDFieldName, uuid},
        {AggregateCommandRequest::kIsClusterQueryWithoutShardKeyCmdFieldName, true},
        {AggregateCommandRequest::kIncludeQueryStatsMetricsFieldName, true},
        {query_request_helper::cmdOptionMaxTimeMS, 10},
        {repl::ReadConcernArgs::kReadConcernFieldName, readConcernObj},
        {query_request_helper::kUnwrappedReadPrefField, readPrefObj}};
    ASSERT_DOCUMENT_EQ(Document(request.toBSON()), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerializeBatchSizeIfSetAndExplainFalse) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    AggregateCommandRequest request(nss, std::vector<mongo::BSONObj>());
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    request.setCursor(cursor);

    auto expectedSerialization = Document{
        {AggregateCommandRequest::kCommandName, nss.coll()},
        {AggregateCommandRequest::kPipelineFieldName, std::vector<Value>{}},
        {AggregateCommandRequest::kCursorFieldName, Value(Document({{kBatchSizeFieldName, 10}}))}};
    ASSERT_DOCUMENT_EQ(Document(request.toBSON()), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerialiseAggregateFieldToOneIfCollectionIsAggregateOneNSS) {
    NamespaceString nss = NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "a"));
    AggregateCommandRequest request(nss, std::vector<mongo::BSONObj>());

    auto expectedSerialization =
        Document{{AggregateCommandRequest::kCommandName, 1},
                 {AggregateCommandRequest::kPipelineFieldName, std::vector<Value>{}},
                 {AggregateCommandRequest::kCursorFieldName,
                  Value(Document({{aggregation_request_helper::kBatchSizeField,
                                   aggregation_request_helper::kDefaultBatchSize}}))}};

    ASSERT_DOCUMENT_EQ(Document(request.toBSON()), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSetBatchSizeToDefaultOnEmptyCursorObject) {
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, $db: 'a'}");
    auto request = aggregation_request_helper::parseFromBSONForTests(inputBson);
    ASSERT_OK(request.getStatus());
    ASSERT_EQ(request.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
}

TEST(AggregationRequestTest, ShouldAcceptHintAsString) {
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], hint: 'a_1', cursor: {}, $db: "
        "'a'}");
    auto request = aggregation_request_helper::parseFromBSONForTests(inputBson);
    ASSERT_OK(request.getStatus());
    ASSERT_BSONOBJ_EQ(request.getValue().getHint().value_or(BSONObj()), BSON("$hint" << "a_1"));
}

TEST(AggregationRequestTest, ShouldNotSerializeBatchSizeWhenExplainSet) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    AggregateCommandRequest request(nss, std::vector<mongo::BSONObj>());
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    request.setCursor(cursor);
    request.setExplain(true);

    auto expectedSerialization =
        Document{{AggregateCommandRequest::kCommandName, nss.coll()},
                 {AggregateCommandRequest::kPipelineFieldName, std::vector<Value>{}},
                 {AggregateCommandRequest::kCursorFieldName, Value(Document())}};
    ASSERT_DOCUMENT_EQ(Document(request.toBSON()), expectedSerialization);
}

//
// Error cases.
//

/**
 * Combines 'validRequest' and 'invalidFields' into a single BSONObj. Note that if the two share
 * a common field, the field from 'invalidFields' will be kept.
 */
BSONObj constructInvalidRequest(const BSONObj& validRequest, const BSONObj& invalidFields) {
    BSONObjBuilder invalidRequestBuilder;

    // An aggregate command expects the first field in the request to be 'aggregate'. As such, we
    // pull out the aggregate field from whichever BSONObj supplied it and append it before any
    // other fields.
    auto validAggregateField = validRequest.getField(AggregateCommandRequest::kCommandName);
    auto invalidAggregateField = invalidFields.getField(AggregateCommandRequest::kCommandName);
    if (!invalidAggregateField.eoo()) {
        invalidRequestBuilder.append(invalidAggregateField);
    } else {
        invariant(!validAggregateField.eoo());
        invalidRequestBuilder.append(validAggregateField);
    }

    // Construct a command object containing a union of the two objects.
    for (auto&& elem : invalidFields) {
        auto fieldName = elem.fieldName();
        if (!invalidRequestBuilder.hasField(fieldName)) {
            invalidRequestBuilder.append(elem);
        }
    }


    for (auto&& elem : validRequest) {
        auto fieldName = elem.fieldName();
        if (!invalidRequestBuilder.hasField(fieldName)) {
            invalidRequestBuilder.append(elem);
        }
    }
    return invalidRequestBuilder.obj();
}

/**
 * Helper which verifies that 'validRequest' parses correctly, but fails to parse once
 * 'invalidFields' are added to it.
 */
void aggregationRequestParseFailureHelper(
    const BSONObj& validRequest,
    const BSONObj& invalidFields,
    ErrorCodes::Error expectedCode,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity = boost::none) {
    // Verify that 'validRequest' parses correctly.
    ASSERT_OK(aggregation_request_helper::parseFromBSONForTests(
                  validRequest, boost::none, explainVerbosity)
                  .getStatus());

    auto invalidRequest = constructInvalidRequest(validRequest, invalidFields);

    // Verify that the constructed invalid request fails to parse.
    auto status = aggregation_request_helper::parseFromBSONForTests(
                      invalidRequest, boost::none, explainVerbosity)
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), expectedCode);
}

// This leverages the IDL generated aggregation namespace parsing.
NamespaceString parseNS(const BSONObj& cmdObj) {
    return uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(cmdObj))
        .getNamespace();
}

/**
 * Verifies that 'validRequest' parses correctly, but throws 'expectedCode' once 'invalidFields'
 * are added to it.
 */
void parseNSHelper(const BSONObj& validRequest,
                   const BSONObj& invalidFields,
                   ErrorCodes::Error expectedCode) {

    // Verify that 'validRequest' parses correctly.
    auto shouldNotThrow = parseNS(validRequest);
    auto invalidRequest = constructInvalidRequest(validRequest, invalidFields);

    // Verify that the constructed invalid request fails to parse with 'expectedCode'.
    ASSERT_THROWS_CODE(parseNS(invalidRequest), AssertionException, expectedCode);
}

TEST(AggregationRequestTest, ShouldRejectNonArrayPipeline) {
    const BSONObj validRequest =
        fromjson("{aggregate: 'collection', pipeline: [], cursor: {}, $db: 'a'}");
    const BSONObj nonArrayPipeline = fromjson("{pipeline: {}}");
    aggregationRequestParseFailureHelper(validRequest, nonArrayPipeline, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectPipelineArrayIfAnElementIsNotAnObject) {
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, $db: 'a'}");
    BSONObj nonObjectPipelineElem = fromjson("{pipeline: [4]}");
    aggregationRequestParseFailureHelper(
        validRequest, nonObjectPipelineElem, ErrorCodes::TypeMismatch);

    nonObjectPipelineElem = fromjson("{pipeline: [{$match: {a: 'abc'}}, 4]}");
    aggregationRequestParseFailureHelper(
        validRequest, nonObjectPipelineElem, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNonObjectCollation) {
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection', "
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {}, "
        "collation: {locale: 'simple'}, "
        "$db: 'a'}");
    const BSONObj nonObjectCollation = fromjson("{collation: 1}");
    aggregationRequestParseFailureHelper(
        validRequest, nonObjectCollation, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNonStringNonObjectHint) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection', "
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {},"
        "hint: {_id: 1},"
        "$db: 'a'}");
    const BSONObj nonObjectHint = fromjson("{hint: 1}");
    aggregationRequestParseFailureHelper(validRequest, nonObjectHint, ErrorCodes::FailedToParse);
}

TEST(AggregationRequestTest, ShouldRejectExplainIfNumber) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {},"
        "explain: true, "
        "$db: 'a'}");
    const BSONObj numericExplain = fromjson("{explain: 1}");
    aggregationRequestParseFailureHelper(validRequest, numericExplain, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectExplainIfObject) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {},"
        "explain: true, "
        "$db: 'a'}");
    const BSONObj objectExplain = fromjson("{explain: {}}");
    aggregationRequestParseFailureHelper(validRequest, objectExplain, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNonBoolFromRouter) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {}, "
        "fromRouter: true, "
        "$db: 'a'}");
    const BSONObj nonBoolFromRouter = fromjson("{fromRouter: 1}");
    aggregationRequestParseFailureHelper(validRequest, nonBoolFromRouter, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNonBoolNeedsMerge) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}], "
        "cursor: {},"
        "needsMerge: true, "
        "fromRouter: true,"
        "$db: 'a'}");
    const BSONObj nonBoolNeedsMerge = fromjson("{needsMerge: 1}");
    aggregationRequestParseFailureHelper(validRequest, nonBoolNeedsMerge, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNonBoolIncludeQueryStatsMetrics) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}], "
        "cursor: {},"
        "includeQueryStatsMetrics: false, "
        "$db: 'a'}");
    const BSONObj nonBoolIncludeQueryStatsMetrics = fromjson("{includeQueryStatsMetrics: 1}");
    aggregationRequestParseFailureHelper(
        validRequest, nonBoolIncludeQueryStatsMetrics, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNeedsMergeIfFromRouterNotPresent) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {}, "
        "$db: 'a'}");
    const BSONObj needsMergeNoFromRouter = fromjson("{needsMerge: true}");
    aggregationRequestParseFailureHelper(
        validRequest, needsMergeNoFromRouter, ErrorCodes::FailedToParse);
}

TEST(AggregationRequestTest, ShouldRejectNonBoolAllowDiskUse) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {},"
        "allowDiskUse: true, "
        "$db: 'a'}");
    const BSONObj nonBoolAllowDiskUse = fromjson("{allowDiskUse: 1}");
    aggregationRequestParseFailureHelper(
        validRequest, nonBoolAllowDiskUse, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNonBoolIsMapReduceCommand) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {}, "
        "isMapReduceCommand: true,"
        "$db: 'a'}");
    const BSONObj nonBoolIsMapReduceCommand = fromjson("{isMapReduceCommand: 1}");
    aggregationRequestParseFailureHelper(
        validRequest, nonBoolIsMapReduceCommand, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectNoCursorNoExplain) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");

    // An aggregate with neither cursor nor explain should fail to parse.
    const BSONObj invalidRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "$db: 'a'}");
    auto status = aggregation_request_helper::parseFromBSONForTests(invalidRequest).getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::FailedToParse);

    // Adding explain should cause the aggregate to parse successfully.
    BSONObjBuilder explainRequest(invalidRequest);
    explainRequest.append("explain", true);
    ASSERT_OK(aggregation_request_helper::parseFromBSONForTests(explainRequest.done()).getStatus());

    // Adding cursor should cause the aggregate to parse successfully.
    BSONObjBuilder cursorRequest(invalidRequest);
    cursorRequest.append("cursor", BSONObj());
    ASSERT_OK(aggregation_request_helper::parseFromBSONForTests(cursorRequest.done()).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonObjectCursor) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {},"
        "isMapReduceCommand: true,"
        "$db: 'a'}");
    const BSONObj nonObjCursorCommand = fromjson("{cursor: 1}");
    aggregationRequestParseFailureHelper(
        validRequest, nonObjCursorCommand, ErrorCodes::TypeMismatch);

    const BSONObj arrayCursorCommand = fromjson("{cursor: []}");
    aggregationRequestParseFailureHelper(
        validRequest, arrayCursorCommand, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectExplainTrueWithSeparateExplainArg) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [],"
        "cursor: {},"
        "$db: 'a'}");
    const BSONObj explainTrue = fromjson("{explain: true}");
    aggregationRequestParseFailureHelper(validRequest,
                                         explainTrue,
                                         ErrorCodes::FailedToParse,
                                         ExplainOptions::Verbosity::kExecStats);
}

TEST(AggregationRequestTest, ShouldRejectExplainFalseWithSeparateExplainArg) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [],"
        "cursor: {},"
        "$db: 'a'}");
    const BSONObj explainFalse = fromjson("{explain: false}");
    aggregationRequestParseFailureHelper(validRequest,
                                         explainFalse,
                                         ErrorCodes::FailedToParse,
                                         ExplainOptions::Verbosity::kExecStats);
}

TEST(AggregationRequestTest, ShouldRejectExplainWithWriteConcernMajority) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest =
        fromjson("{aggregate: 'collection', pipeline: [], explain: true, $db: 'a'}");
    const BSONObj wcMajority = fromjson("{writeConcern: {w: 'majority'}}");
    aggregationRequestParseFailureHelper(validRequest, wcMajority, ErrorCodes::FailedToParse);
}

TEST(AggregationRequestTest, ShouldRejectExplainExecStatsVerbosityWithWriteConcernMajority) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest =
        fromjson("{aggregate: 'collection', pipeline: [], cursor: {}, $db: 'a'}");
    const BSONObj wcMajority = fromjson("{writeConcern: {w: 'majority'}}");
    aggregationRequestParseFailureHelper(
        validRequest, wcMajority, ErrorCodes::FailedToParse, ExplainOptions::Verbosity::kExecStats);
}

TEST(AggregationRequestTest, ShouldRejectRequestReshardingResumeTokenIfNonBooleanType) {
    const BSONObj validRequest = fromjson(
        "{aggregate: 'oplog.rs',"
        "pipeline: [],"
        "$_requestReshardingResumeToken: true,"
        "$db: 'local', "
        "cursor: {}}");
    const BSONObj nonBoolReshardingResumeToken =
        fromjson("{$_requestReshardingResumeToken: 'yes'}");
    aggregationRequestParseFailureHelper(
        validRequest, nonBoolReshardingResumeToken, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectRequestReshardingResumeTokenIfNonOplogNss) {
    const BSONObj validRequest = fromjson(
        "{aggregate: 'oplog.rs',"
        "pipeline: [],"
        "$_requestReshardingResumeToken: true,"
        "$db: 'local', "
        "cursor: {}}");
    const BSONObj nonOplogNss = fromjson("{aggregate: 'collection', $db: 'a'}");
    aggregationRequestParseFailureHelper(validRequest, nonOplogNss, ErrorCodes::FailedToParse);
}

TEST(AggregationRequestTest, ParseNSShouldReturnAggregateOneNSIfAggregateFieldIsOne) {
    const std::vector<std::string> ones{
        "1", "1.0", "NumberInt(1)", "NumberLong(1)", "NumberDecimal('1')"};

    for (auto& one : ones) {
        const BSONObj inputBSON = fromjson(
            str::stream() << "{aggregate: " << one << ", pipeline: [], $db: 'a', cursor: {}}");
        ASSERT(parseNS(inputBSON).isCollectionlessAggregateNS());
    }
}

TEST(AggregationRequestTest, ParseNSShouldRejectNumericNSIfAggregateFieldIsNotOne) {
    const BSONObj validRequest = fromjson("{aggregate: 1, pipeline: [], $db: 'a', cursor: {}}");
    BSONObj nonOneAggregate = fromjson("{aggregate: 2}");
    parseNSHelper(validRequest, nonOneAggregate, ErrorCodes::BadValue);
}

TEST(AggregationRequestTest, ParseNSShouldRejectNonStringNonNumericNS) {
    const BSONObj validRequest = fromjson("{aggregate: 1, pipeline: [], $db: 'a', cursor: {}}");
    BSONObj nonStringNonNumericNS = fromjson("{aggregate: {}}");
    parseNSHelper(validRequest, nonStringNonNumericNS, ErrorCodes::InvalidNamespace);
}

TEST(AggregationRequestTest, ParseNSShouldRejectAggregateOneStringAsCollectionName) {
    const BSONObj validRequest = fromjson("{aggregate: 1, pipeline: [], $db: 'a', cursor: {}}");
    BSONObj oneStringAsCollectionName = fromjson("{aggregate: '$cmd.aggregate'}");
    parseNSHelper(validRequest, oneStringAsCollectionName, ErrorCodes::InvalidNamespace);
}

TEST(AggregationRequestTest, ParseNSShouldRejectInvalidCollectionName) {
    const BSONObj validRequest = fromjson("{aggregate: 1, pipeline: [], $db: 'a', cursor: {}}");
    BSONObj invalidCollectionName = fromjson("{aggregate: ''}");
    parseNSHelper(validRequest, invalidCollectionName, ErrorCodes::InvalidNamespace);
}

TEST(AggregationRequestTest, ShouldRejectExchangeNotObject) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection', "
        "pipeline: [],"
        "cursor: {},"
        "exchange: {policy: 'roundrobin', consumers: 2},"
        "$db: 'a'}");
    const BSONObj nonObjectExchange = fromjson("{exchange: '42'}");
    aggregationRequestParseFailureHelper(validRequest, nonObjectExchange, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectExchangeInvalidSpec) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [],"
        "cursor: {},"
        "exchange: {policy: 'roundrobin', consumers: 2}, "
        "$db: 'a'}");
    const BSONObj invalidExchangeSpec = fromjson("{exchange: {}}");
    aggregationRequestParseFailureHelper(
        validRequest, invalidExchangeSpec, ErrorCodes::IDLFailedToParse);
}

TEST(AggregationRequestTest, ShouldRejectInvalidWriteConcern) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [{$match: {a: 'abc'}}],"
        "cursor: {},"
        "writeConcern: {w: 'majority'}, "
        "$db: 'a'}");
    const BSONObj invalidWC = fromjson("{writeConcern: 'invalid'}");
    aggregationRequestParseFailureHelper(validRequest, invalidWC, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectInvalidCollectionUUID) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    auto uuid = UUID::gen();
    BSONObjBuilder validRequestBuilder(
        fromjson("{aggregate: 'collection', cursor: {}, pipeline: [{$match: {}}], $db: 'a'}"));
    uuid.appendToBuilder(&validRequestBuilder, AggregateCommandRequest::kCollectionUUIDFieldName);
    const BSONObj validRequest = validRequestBuilder.done();
    const BSONObj invalidCollectionUUID = fromjson("{collectionUUID: 2}");
    aggregationRequestParseFailureHelper(
        validRequest, invalidCollectionUUID, ErrorCodes::TypeMismatch);
}

//
// Ignore fields parsed elsewhere.
//

TEST(AggregationRequestTest, ShouldRejectUnknownField) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection', "
        "pipeline: [],"
        "cursor: {},"
        "$db: 'a'}");
    const BSONObj nonObjectExchange =
        fromjson("{thisIsNotARealField: 'this is not a real option'}");
    auto unknownFieldErrorCode = ErrorCodes::duplicateCodeForTest(40415);
    aggregationRequestParseFailureHelper(validRequest, nonObjectExchange, unknownFieldErrorCode);
}

TEST(AggregationRequestTest, ShouldIgnoreQueryOptions) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, $queryOptions: "
        "{}, $db: 'a'}");
    ASSERT_OK(aggregation_request_helper::parseFromBSONForTests(inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectRequestResumeTokenIfNonBooleanType) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.collection");
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [],"
        "$_requestResumeToken: true,"
        "hint: {$natural: 1},"
        "$db: 'a', "
        "cursor: {}}");
    const BSONObj nonBoolReshardingResumeToken = fromjson("{$_requestResumeToken: 'yes'}");
    aggregationRequestParseFailureHelper(
        validRequest, nonBoolReshardingResumeToken, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ShouldRejectRequestResumeTokenIfOplogNss) {
    const BSONObj validRequest = fromjson(
        "{aggregate: 'collection',"
        "pipeline: [],"
        "$_requestResumeToken: true,"
        "hint: {$natural: 1},"
        "$db: 'a', "
        "cursor: {}}");
    const BSONObj oplogNss = fromjson("{aggregate: 'oplog.rs', $db: 'local'}");
    aggregationRequestParseFailureHelper(validRequest, oplogNss, ErrorCodes::FailedToParse);
}

}  // namespace
}  // namespace mongo
