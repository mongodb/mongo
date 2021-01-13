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

#include "mongo/db/pipeline/aggregation_request_helper.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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
    NamespaceString nss("oplog.rs");
    BSONObj inputBson = fromjson(
        "{aggregate: 'oplog.rs', pipeline: [{$match: {a: 'abc'}}], explain: false, allowDiskUse: "
        "true, fromMongos: true, "
        "needsMerge: true, bypassDocumentValidation: true, $_requestReshardingResumeToken: true, "
        "collation: {locale: 'en_US'}, cursor: {batchSize: 10}, hint: {a: 1}, maxTimeMS: 100, "
        "readConcern: {level: 'linearizable'}, $queryOptions: {$readPreference: 'nearest'}, "
        "exchange: {policy: 'roundrobin', consumers:NumberInt(2)}, isMapReduceCommand: true, $db: "
        "'local'}");
    auto uuid = UUID::gen();
    BSONObjBuilder uuidBob;
    uuid.appendToBuilder(&uuidBob, AggregateCommand::kCollectionUUIDFieldName);
    inputBson = inputBson.addField(uuidBob.obj().firstElement());

    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSON(nss, inputBson));
    ASSERT_FALSE(request.getExplain());
    ASSERT_TRUE(request.getAllowDiskUse());
    ASSERT_TRUE(request.getFromMongos());
    ASSERT_TRUE(request.getNeedsMerge());
    ASSERT_TRUE(request.getBypassDocumentValidation().value_or(false));
    ASSERT_TRUE(request.getRequestReshardingResumeToken());
    ASSERT_EQ(
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        10);
    ASSERT_BSONOBJ_EQ(request.getHint().value_or(BSONObj()), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(request.getCollation().value_or(BSONObj()),
                      BSON("locale"
                           << "en_US"));
    ASSERT_EQ(*request.getMaxTimeMS(), 100u);
    ASSERT_BSONOBJ_EQ(*request.getReadConcern(),
                      BSON("level"
                           << "linearizable"));
    ASSERT_BSONOBJ_EQ(request.getUnwrappedReadPref().value_or(BSONObj()),
                      BSON("$readPreference"
                           << "nearest"));
    ASSERT_TRUE(request.getExchange().is_initialized());
    ASSERT_TRUE(request.getIsMapReduceCommand());
    ASSERT_EQ(*request.getCollectionUUID(), uuid);
}

TEST(AggregationRequestTest, ShouldParseExplicitRequestReshardingResumeTokenFalseForNonOplog) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], $_requestReshardingResumeToken: false, cursor: "
        "{}, $db: 'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSON(nss, inputBson));
    ASSERT_FALSE(request.getRequestReshardingResumeToken());
}

TEST(AggregationRequestTest, ShouldParseExplicitExplainTrue) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], explain: true, cursor: {}, $db: 'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSON(nss, inputBson));
    ASSERT_TRUE(request.getExplain());
    ASSERT(*request.getExplain() == ExplainOptions::Verbosity::kQueryPlanner);
}

TEST(AggregationRequestTest, ShouldParseExplicitExplainFalseWithCursorOption) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], explain: false, cursor: {batchSize: 10}, $db: "
        "'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSON(nss, inputBson));
    ASSERT_FALSE(request.getExplain());
    ASSERT_EQ(
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        10);
}

TEST(AggregationRequestTest, ShouldParseWithSeparateQueryPlannerExplainModeArg) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], cursor: {}, $db: 'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSON(
        nss, inputBson, ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_TRUE(request.getExplain());
    ASSERT(*request.getExplain() == ExplainOptions::Verbosity::kQueryPlanner);
}

TEST(AggregationRequestTest, ShouldParseWithSeparateQueryPlannerExplainModeArgAndCursorOption) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], cursor: {batchSize: 10}, $db: 'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSON(
        nss, inputBson, ExplainOptions::Verbosity::kExecStats));
    ASSERT_TRUE(request.getExplain());
    ASSERT(*request.getExplain() == ExplainOptions::Verbosity::kExecStats);
    ASSERT_EQ(
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        10);
}

TEST(AggregationRequestTest, ShouldParseExplainFlagWithReadConcern) {
    NamespaceString nss("a.collection");
    // Non-local readConcern should not be allowed with the explain flag, but this is checked
    // elsewhere to avoid having to parse the readConcern in AggregationCommand.
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], explain: true, readConcern: {level: 'majority'}, "
        "$db: 'a'}");
    auto request = unittest::assertGet(aggregation_request_helper::parseFromBSON(nss, inputBson));
    ASSERT_TRUE(request.getExplain());
    ASSERT_BSONOBJ_EQ(*request.getReadConcern(),
                      BSON("level"
                           << "majority"));
}

//
// Serialization
//

TEST(AggregationRequestTest, ShouldOnlySerializeRequiredFieldsIfNoOptionalFieldsAreSpecified) {
    NamespaceString nss("a.collection");
    AggregateCommand request(nss, {});

    auto expectedSerialization =
        Document{{AggregateCommand::kCommandName, nss.coll()},
                 {AggregateCommand::kPipelineFieldName, std::vector<Value>{}},
                 {AggregateCommand::kCursorFieldName, Value(kDefaultCursorOptionDocument)}};
    ASSERT_DOCUMENT_EQ(aggregation_request_helper::serializeToCommandDoc(request),
                       expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerializeOptionalValuesIfSet) {
    NamespaceString nss("a.collection");
    AggregateCommand request(nss, {});
    request.setAllowDiskUse(true);
    request.setFromMongos(true);
    request.setNeedsMerge(true);
    request.setBypassDocumentValidation(true);
    request.setRequestReshardingResumeToken(true);
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    request.setCursor(cursor);
    request.setMaxTimeMS(10u);
    const auto hintObj = BSON("a" << 1);
    request.setHint(hintObj);
    const auto collationObj = BSON("locale"
                                   << "en_US");
    request.setCollation(collationObj);
    const auto readPrefObj = BSON("$readPreference"
                                  << "nearest");
    request.setUnwrappedReadPref(readPrefObj);
    const auto readConcernObj = BSON("level"
                                     << "linearizable");
    request.setReadConcern(readConcernObj);
    request.setIsMapReduceCommand(true);
    const auto letParamsObj = BSON("foo"
                                   << "bar");
    request.setLet(letParamsObj);
    auto uuid = UUID::gen();
    request.setCollectionUUID(uuid);

    auto expectedSerialization =
        Document{{AggregateCommand::kCommandName, nss.coll()},
                 {AggregateCommand::kPipelineFieldName, std::vector<Value>{}},
                 {AggregateCommand::kAllowDiskUseFieldName, true},
                 {AggregateCommand::kCursorFieldName, Value(Document({{kBatchSizeFieldName, 10}}))},
                 {QueryRequest::cmdOptionMaxTimeMS, 10},
                 {AggregateCommand::kBypassDocumentValidationFieldName, true},
                 {repl::ReadConcernArgs::kReadConcernFieldName, readConcernObj},
                 {AggregateCommand::kCollationFieldName, collationObj},
                 {AggregateCommand::kHintFieldName, hintObj},
                 {AggregateCommand::kLetFieldName, letParamsObj},
                 {AggregateCommand::kNeedsMergeFieldName, true},
                 {AggregateCommand::kFromMongosFieldName, true},
                 {QueryRequest::kUnwrappedReadPrefField, readPrefObj},
                 {AggregateCommand::kRequestReshardingResumeTokenFieldName, true},
                 {AggregateCommand::kIsMapReduceCommandFieldName, true},
                 {AggregateCommand::kCollectionUUIDFieldName, uuid}};
    ASSERT_DOCUMENT_EQ(aggregation_request_helper::serializeToCommandDoc(request),
                       expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerializeBatchSizeIfSetAndExplainFalse) {
    NamespaceString nss("a.collection");
    AggregateCommand request(nss, {});
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    request.setCursor(cursor);

    auto expectedSerialization = Document{
        {AggregateCommand::kCommandName, nss.coll()},
        {AggregateCommand::kPipelineFieldName, std::vector<Value>{}},
        {AggregateCommand::kCursorFieldName, Value(Document({{kBatchSizeFieldName, 10}}))}};
    ASSERT_DOCUMENT_EQ(aggregation_request_helper::serializeToCommandDoc(request),
                       expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerialiseAggregateFieldToOneIfCollectionIsAggregateOneNSS) {
    NamespaceString nss = NamespaceString::makeCollectionlessAggregateNSS("a");
    AggregateCommand request(nss, {});

    auto expectedSerialization =
        Document{{AggregateCommand::kCommandName, 1},
                 {AggregateCommand::kPipelineFieldName, std::vector<Value>{}},
                 {AggregateCommand::kCursorFieldName,
                  Value(Document({{aggregation_request_helper::kBatchSizeField,
                                   aggregation_request_helper::kDefaultBatchSize}}))}};

    ASSERT_DOCUMENT_EQ(aggregation_request_helper::serializeToCommandDoc(request),
                       expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSetBatchSizeToDefaultOnEmptyCursorObject) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, $db: 'a'}");
    auto request = aggregation_request_helper::parseFromBSON(nss, inputBson);
    ASSERT_OK(request.getStatus());
    ASSERT_EQ(request.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
}

TEST(AggregationRequestTest, ShouldAcceptHintAsString) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], hint: 'a_1', cursor: {}, $db: "
        "'a'}");
    auto request = aggregation_request_helper::parseFromBSON(nss, inputBson);
    ASSERT_OK(request.getStatus());
    ASSERT_BSONOBJ_EQ(request.getValue().getHint().value_or(BSONObj()),
                      BSON("$hint"
                           << "a_1"));
}

TEST(AggregationRequestTest, ShouldNotSerializeBatchSizeWhenExplainSet) {
    NamespaceString nss("a.collection");
    AggregateCommand request(nss, {});
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    request.setCursor(cursor);
    request.setExplain(ExplainOptions::Verbosity::kQueryPlanner);

    auto expectedSerialization =
        Document{{AggregateCommand::kCommandName, nss.coll()},
                 {AggregateCommand::kPipelineFieldName, std::vector<Value>{}},
                 {AggregateCommand::kCursorFieldName, Value(Document())}};
    ASSERT_DOCUMENT_EQ(aggregation_request_helper::serializeToCommandDoc(request),
                       expectedSerialization);
}

//
// Error cases.
//

TEST(AggregationRequestTest, ShouldRejectNonArrayPipeline) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: {}, cursor: {}, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectPipelineArrayIfAnElementIsNotAnObject) {
    NamespaceString nss("a.collection");
    BSONObj inputBson = fromjson("{aggregate: 'collection', pipeline: [4], cursor: {}, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());

    inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}, 4], cursor: {}, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonObjectCollation) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, collation: 1, "
        "$db: 'a'}");
    ASSERT_NOT_OK(
        aggregation_request_helper::parseFromBSON(NamespaceString("a.collection"), inputBson)
            .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonStringNonObjectHint) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, hint: 1, $db: "
        "'a'}");
    ASSERT_NOT_OK(
        aggregation_request_helper::parseFromBSON(NamespaceString("a.collection"), inputBson)
            .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectHintAsArray) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, hint: [], $db: "
        "'a'}]}");
    ASSERT_NOT_OK(
        aggregation_request_helper::parseFromBSON(NamespaceString("a.collection"), inputBson)
            .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainIfNumber) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, explain: 1, $db: "
        "'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainIfObject) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, explain: {}, $db: "
        "'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonBoolFromMongos) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, fromMongos: 1, "
        "$db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonBoolNeedsMerge) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, needsMerge: 1, "
        "fromMongos: true, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNeedsMergeIfFromMongosNotPresent) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, needsMerge: true, "
        "$db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonBoolNeedsMerge34) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, fromRouter: 1, "
        "$db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNeedsMergeIfNeedsMerge34AlsoPresent) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, needsMerge: true, "
        "fromMongos: true, "
        "fromRouter: true, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectFromMongosIfNeedsMerge34AlsoPresent) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, fromMongos: true, "
        "fromRouter: true, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonBoolAllowDiskUse) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, allowDiskUse: 1, "
        "$db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonBoolIsMapReduceCommand) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, "
        "isMapReduceCommand: 1, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNoCursorNoExplain) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainTrueWithSeparateExplainArg) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], explain: true, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(
                      nss, inputBson, ExplainOptions::Verbosity::kExecStats)
                      .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainFalseWithSeparateExplainArg) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], explain: false, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(
                      nss, inputBson, ExplainOptions::Verbosity::kExecStats)
                      .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainExecStatsVerbosityWithReadConcernMajority) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], readConcern: {level: 'majority'}, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(
                      nss, inputBson, ExplainOptions::Verbosity::kExecStats)
                      .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainWithWriteConcernMajority) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], explain: true, writeConcern: {w: 'majority'}, "
        "$db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainExecStatsVerbosityWithWriteConcernMajority) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], writeConcern: {w: 'majority'}, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(
                      nss, inputBson, ExplainOptions::Verbosity::kExecStats)
                      .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectRequestReshardingResumeTokenIfNonOplogNss) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [], $_requestReshardingResumeToken: true, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, CannotParseNeedsMerge34) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, fromRouter: true, "
        "$db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ParseNSShouldReturnAggregateOneNSIfAggregateFieldIsOne) {
    const std::vector<std::string> ones{
        "1", "1.0", "NumberInt(1)", "NumberLong(1)", "NumberDecimal('1')"};

    for (auto& one : ones) {
        const BSONObj inputBSON =
            fromjson(str::stream() << "{aggregate: " << one << ", pipeline: [], $db: 'a'}");
        ASSERT(aggregation_request_helper::parseNs("a", inputBSON).isCollectionlessAggregateNS());
    }
}

TEST(AggregationRequestTest, ParseNSShouldRejectNumericNSIfAggregateFieldIsNotOne) {
    const BSONObj inputBSON = fromjson("{aggregate: 2, pipeline: [], $db: 'a'}");
    ASSERT_THROWS_CODE(aggregation_request_helper::parseNs("a", inputBSON),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST(AggregationRequestTest, ParseNSShouldRejectNonStringNonNumericNS) {
    const BSONObj inputBSON = fromjson("{aggregate: {}, pipeline: [], $db: 'a'}");
    ASSERT_THROWS_CODE(aggregation_request_helper::parseNs("a", inputBSON),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ParseNSShouldRejectAggregateOneStringAsCollectionName) {
    const BSONObj inputBSON = fromjson("{aggregate: '$cmd.aggregate', pipeline: [], $db: 'a'}");
    ASSERT_THROWS_CODE(aggregation_request_helper::parseNs("a", inputBSON),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST(AggregationRequestTest, ParseNSShouldRejectInvalidCollectionName) {
    const BSONObj inputBSON = fromjson("{aggregate: '', pipeline: [], $db: 'a'}");
    ASSERT_THROWS_CODE(aggregation_request_helper::parseNs("a", inputBSON),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST(AggregationRequestTest, ParseFromBSONOverloadsShouldProduceIdenticalRequests) {
    const BSONObj inputBSON = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {}}, {$project: {}}], cursor: {}, $db: "
        "'a'}");
    NamespaceString nss("a.collection");

    auto aggReqDBName =
        unittest::assertGet(aggregation_request_helper::parseFromBSON("a", inputBSON));
    auto aggReqNSS = unittest::assertGet(aggregation_request_helper::parseFromBSON(nss, inputBSON));

    ASSERT_DOCUMENT_EQ(aggregation_request_helper::serializeToCommandDoc(aggReqDBName),
                       aggregation_request_helper::serializeToCommandDoc(aggReqNSS));
}

TEST(AggregationRequestTest, ShouldRejectExchangeNotObject) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], exchage: '42', $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExchangeInvalidSpec) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{aggregate: 'collection', pipeline: [], exchage: {}, $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectInvalidWriteConcern) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, writeConcern: "
        "'invalid', $db: 'a'}");
    ASSERT_NOT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectInvalidCollectionUUID) {
    NamespaceString nss("a.collection");
    const BSONObj inputBSON = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {}}], collectionUUID: 2, $db: 'a'}");
    ASSERT_EQUALS(aggregation_request_helper::parseFromBSON(nss, inputBSON).getStatus().code(),
                  ErrorCodes::TypeMismatch);
}

//
// Ignore fields parsed elsewhere.
//

TEST(AggregationRequestTest, ShouldIgnoreQueryOptions) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{aggregate: 'collection', pipeline: [{$match: {a: 'abc'}}], cursor: {}, $queryOptions: "
        "{}, $db: 'a'}");
    ASSERT_OK(aggregation_request_helper::parseFromBSON(nss, inputBson).getStatus());
}

}  // namespace
}  // namespace mongo
