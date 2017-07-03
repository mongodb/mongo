/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_request.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

const Document kDefaultCursorOptionDocument{
    {AggregationRequest::kBatchSizeName, AggregationRequest::kDefaultBatchSize}};

//
// Parsing
//

TEST(AggregationRequestTest, ShouldParseAllKnownOptions) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson(
        "{pipeline: [{$match: {a: 'abc'}}], explain: false, allowDiskUse: true, fromRouter: true, "
        "bypassDocumentValidation: true, collation: {locale: 'en_US'}, cursor: {batchSize: 10}, "
        "hint: {a: 1}, maxTimeMS: 100, readConcern: {level: 'linearizable'}, "
        "$queryOptions: {$readPreference: 'nearest'}, comment: 'agg_comment'}}");
    auto request = unittest::assertGet(AggregationRequest::parseFromBSON(nss, inputBson));
    ASSERT_FALSE(request.getExplain());
    ASSERT_TRUE(request.shouldAllowDiskUse());
    ASSERT_TRUE(request.isFromRouter());
    ASSERT_TRUE(request.shouldBypassDocumentValidation());
    ASSERT_EQ(request.getBatchSize(), 10);
    ASSERT_BSONOBJ_EQ(request.getHint(), BSON("a" << 1));
    ASSERT_EQ(request.getComment(), "agg_comment");
    ASSERT_BSONOBJ_EQ(request.getCollation(),
                      BSON("locale"
                           << "en_US"));
    ASSERT_EQ(request.getMaxTimeMS(), 100u);
    ASSERT_BSONOBJ_EQ(request.getReadConcern(),
                      BSON("level"
                           << "linearizable"));
    ASSERT_BSONOBJ_EQ(request.getUnwrappedReadPref(),
                      BSON("$readPreference"
                           << "nearest"));
}

TEST(AggregationRequestTest, ShouldParseExplicitExplainTrue) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], explain: true, cursor: {}}");
    auto request = unittest::assertGet(AggregationRequest::parseFromBSON(nss, inputBson));
    ASSERT_TRUE(request.getExplain());
    ASSERT(*request.getExplain() == ExplainOptions::Verbosity::kQueryPlanner);
}

TEST(AggregationRequestTest, ShouldParseExplicitExplainFalseWithCursorOption) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], explain: false, cursor: {batchSize: 10}}");
    auto request = unittest::assertGet(AggregationRequest::parseFromBSON(nss, inputBson));
    ASSERT_FALSE(request.getExplain());
    ASSERT_EQ(request.getBatchSize(), 10);
}

TEST(AggregationRequestTest, ShouldParseWithSeparateQueryPlannerExplainModeArg) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], cursor: {}}");
    auto request = unittest::assertGet(AggregationRequest::parseFromBSON(
        nss, inputBson, ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_TRUE(request.getExplain());
    ASSERT(*request.getExplain() == ExplainOptions::Verbosity::kQueryPlanner);
}

TEST(AggregationRequestTest, ShouldParseWithSeparateQueryPlannerExplainModeArgAndCursorOption) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], cursor: {batchSize: 10}}");
    auto request = unittest::assertGet(
        AggregationRequest::parseFromBSON(nss, inputBson, ExplainOptions::Verbosity::kExecStats));
    ASSERT_TRUE(request.getExplain());
    ASSERT(*request.getExplain() == ExplainOptions::Verbosity::kExecStats);
    ASSERT_EQ(request.getBatchSize(), 10);
}

//
// Serialization
//

TEST(AggregationRequestTest, ShouldOnlySerializeRequiredFieldsIfNoOptionalFieldsAreSpecified) {
    NamespaceString nss("a.collection");
    AggregationRequest request(nss, {});

    auto expectedSerialization =
        Document{{AggregationRequest::kCommandName, nss.coll()},
                 {AggregationRequest::kPipelineName, Value(std::vector<Value>{})},
                 {AggregationRequest::kCursorName, Value(kDefaultCursorOptionDocument)}};
    ASSERT_DOCUMENT_EQ(request.serializeToCommandObj(), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldNotSerializeOptionalValuesIfEquivalentToDefault) {
    NamespaceString nss("a.collection");
    AggregationRequest request(nss, {});
    request.setExplain(boost::none);
    request.setAllowDiskUse(false);
    request.setFromRouter(false);
    request.setBypassDocumentValidation(false);
    request.setCollation(BSONObj());
    request.setHint(BSONObj());
    request.setComment("");
    request.setMaxTimeMS(0u);
    request.setUnwrappedReadPref(BSONObj());
    request.setReadConcern(BSONObj());

    auto expectedSerialization =
        Document{{AggregationRequest::kCommandName, nss.coll()},
                 {AggregationRequest::kPipelineName, Value(std::vector<Value>{})},
                 {AggregationRequest::kCursorName, Value(kDefaultCursorOptionDocument)}};
    ASSERT_DOCUMENT_EQ(request.serializeToCommandObj(), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerializeOptionalValuesIfSet) {
    NamespaceString nss("a.collection");
    AggregationRequest request(nss, {});
    request.setAllowDiskUse(true);
    request.setFromRouter(true);
    request.setBypassDocumentValidation(true);
    request.setBatchSize(10);
    request.setMaxTimeMS(10u);
    const auto hintObj = BSON("a" << 1);
    request.setHint(hintObj);
    const auto comment = std::string("agg_comment");
    request.setComment(comment);
    const auto collationObj = BSON("locale"
                                   << "en_US");
    request.setCollation(collationObj);
    const auto readPrefObj = BSON("$readPreference"
                                  << "nearest");
    request.setUnwrappedReadPref(readPrefObj);
    const auto readConcernObj = BSON("level"
                                     << "linearizable");
    request.setReadConcern(readConcernObj);

    auto expectedSerialization =
        Document{{AggregationRequest::kCommandName, nss.coll()},
                 {AggregationRequest::kPipelineName, Value(std::vector<Value>{})},
                 {AggregationRequest::kAllowDiskUseName, true},
                 {AggregationRequest::kFromRouterName, true},
                 {bypassDocumentValidationCommandOption(), true},
                 {AggregationRequest::kCollationName, collationObj},
                 {AggregationRequest::kCursorName,
                  Value(Document({{AggregationRequest::kBatchSizeName, 10}}))},
                 {AggregationRequest::kHintName, hintObj},
                 {AggregationRequest::kCommentName, comment},
                 {repl::ReadConcernArgs::kReadConcernFieldName, readConcernObj},
                 {QueryRequest::kUnwrappedReadPrefField, readPrefObj},
                 {QueryRequest::cmdOptionMaxTimeMS, 10}};
    ASSERT_DOCUMENT_EQ(request.serializeToCommandObj(), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerializeBatchSizeIfSetAndExplainFalse) {
    NamespaceString nss("a.collection");
    AggregationRequest request(nss, {});
    request.setBatchSize(10);

    auto expectedSerialization =
        Document{{AggregationRequest::kCommandName, nss.coll()},
                 {AggregationRequest::kPipelineName, Value(std::vector<Value>{})},
                 {AggregationRequest::kCursorName,
                  Value(Document({{AggregationRequest::kBatchSizeName, 10}}))}};
    ASSERT_DOCUMENT_EQ(request.serializeToCommandObj(), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSerialiseAggregateFieldToOneIfCollectionIsAggregateOneNSS) {
    NamespaceString nss = NamespaceString::makeCollectionlessAggregateNSS("a");
    AggregationRequest request(nss, {});

    auto expectedSerialization =
        Document{{AggregationRequest::kCommandName, 1},
                 {AggregationRequest::kPipelineName, Value(std::vector<Value>{})},
                 {AggregationRequest::kCursorName,
                  Value(Document({{AggregationRequest::kBatchSizeName,
                                   AggregationRequest::kDefaultBatchSize}}))}};

    ASSERT_DOCUMENT_EQ(request.serializeToCommandObj(), expectedSerialization);
}

TEST(AggregationRequestTest, ShouldSetBatchSizeToDefaultOnEmptyCursorObject) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}}");
    auto request = AggregationRequest::parseFromBSON(nss, inputBson);
    ASSERT_OK(request.getStatus());
    ASSERT_EQ(request.getValue().getBatchSize(), AggregationRequest::kDefaultBatchSize);
}

TEST(AggregationRequestTest, ShouldAcceptHintAsString) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], hint: 'a_1', cursor: {}}");
    auto request = AggregationRequest::parseFromBSON(nss, inputBson);
    ASSERT_OK(request.getStatus());
    ASSERT_BSONOBJ_EQ(request.getValue().getHint(),
                      BSON("$hint"
                           << "a_1"));
}

TEST(AggregationRequestTest, ShouldNotSerializeBatchSizeWhenExplainSet) {
    NamespaceString nss("a.collection");
    AggregationRequest request(nss, {});
    request.setBatchSize(10);
    request.setExplain(ExplainOptions::Verbosity::kQueryPlanner);

    auto expectedSerialization =
        Document{{AggregationRequest::kCommandName, nss.coll()},
                 {AggregationRequest::kPipelineName, Value(std::vector<Value>{})},
                 {AggregationRequest::kCursorName, Value(Document())}};
    ASSERT_DOCUMENT_EQ(request.serializeToCommandObj(), expectedSerialization);
}

//
// Error cases.
//

TEST(AggregationRequestTest, ShouldRejectNonArrayPipeline) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: {}, cursor: {}}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectPipelineArrayIfAnElementIsNotAnObject) {
    NamespaceString nss("a.collection");
    BSONObj inputBson = fromjson("{pipeline: [4], cursor: {}}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());

    inputBson = fromjson("{pipeline: [{$match: {a: 'abc'}}, 4], cursor: {}}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonObjectCollation) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, collation: 1}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(NamespaceString("a.collection"), inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonStringNonObjectHint) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, hint: 1}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(NamespaceString("a.collection"), inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectHintAsArray) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, hint: []}]}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(NamespaceString("a.collection"), inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonStringComment) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, comment: 1}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(NamespaceString("a.collection"), inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainIfNumber) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, explain: 1}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainIfObject) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, explain: {}}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonBoolFromRouter) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, fromRouter: 1}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNonBoolAllowDiskUse) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, allowDiskUse: 1}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectNoCursorNoExplain) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [{$match: {a: 'abc'}}]}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainTrueWithSeparateExplainArg) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], explain: true}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(nss, inputBson, ExplainOptions::Verbosity::kExecStats)
            .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainFalseWithSeparateExplainArg) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], explain: false}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(nss, inputBson, ExplainOptions::Verbosity::kExecStats)
            .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainWithReadConcernMajority) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [], explain: true, readConcern: {level: 'majority'}}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainExecStatsVerbosityWithReadConcernMajority) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], readConcern: {level: 'majority'}}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(nss, inputBson, ExplainOptions::Verbosity::kExecStats)
            .getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainWithWriteConcernMajority) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [], explain: true, writeConcern: {w: 'majority'}}");
    ASSERT_NOT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldRejectExplainExecStatsVerbosityWithWriteConcernMajority) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson = fromjson("{pipeline: [], writeConcern: {w: 'majority'}}");
    ASSERT_NOT_OK(
        AggregationRequest::parseFromBSON(nss, inputBson, ExplainOptions::Verbosity::kExecStats)
            .getStatus());
}

TEST(AggregationRequestTest, ParseNSShouldReturnAggregateOneNSIfAggregateFieldIsOne) {
    const std::vector<std::string> ones{
        "1", "1.0", "NumberInt(1)", "NumberLong(1)", "NumberDecimal('1')"};

    for (auto& one : ones) {
        const BSONObj inputBSON =
            fromjson(str::stream() << "{aggregate: " << one << ", pipeline: []}");
        ASSERT(AggregationRequest::parseNs("a", inputBSON).isCollectionlessAggregateNS());
    }
}

TEST(AggregationRequestTest, ParseNSShouldRejectNumericNSIfAggregateFieldIsNotOne) {
    const BSONObj inputBSON = fromjson("{aggregate: 2, pipeline: []}");
    ASSERT_THROWS_CODE(
        AggregationRequest::parseNs("a", inputBSON), UserException, ErrorCodes::FailedToParse);
}

TEST(AggregationRequestTest, ParseNSShouldRejectNonStringNonNumericNS) {
    const BSONObj inputBSON = fromjson("{aggregate: {}, pipeline: []}");
    ASSERT_THROWS_CODE(
        AggregationRequest::parseNs("a", inputBSON), UserException, ErrorCodes::TypeMismatch);
}

TEST(AggregationRequestTest, ParseNSShouldRejectAggregateOneStringAsCollectionName) {
    const BSONObj inputBSON = fromjson("{aggregate: '$cmd.aggregate', pipeline: []}");
    ASSERT_THROWS_CODE(
        AggregationRequest::parseNs("a", inputBSON), UserException, ErrorCodes::InvalidNamespace);
}

TEST(AggregationRequestTest, ParseNSShouldRejectInvalidCollectionName) {
    const BSONObj inputBSON = fromjson("{aggregate: '', pipeline: []}");
    ASSERT_THROWS_CODE(
        AggregationRequest::parseNs("a", inputBSON), UserException, ErrorCodes::InvalidNamespace);
}

TEST(AggregationRequestTest, ParseFromBSONOverloadsShouldProduceIdenticalRequests) {
    const BSONObj inputBSON =
        fromjson("{aggregate: 'collection', pipeline: [{$match: {}}, {$project: {}}], cursor: {}}");
    NamespaceString nss("a.collection");

    auto aggReqDBName = unittest::assertGet(AggregationRequest::parseFromBSON("a", inputBSON));
    auto aggReqNSS = unittest::assertGet(AggregationRequest::parseFromBSON(nss, inputBSON));

    ASSERT_DOCUMENT_EQ(aggReqDBName.serializeToCommandObj(), aggReqNSS.serializeToCommandObj());
}

//
// Ignore fields parsed elsewhere.
//

TEST(AggregationRequestTest, ShouldIgnoreQueryOptions) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, $queryOptions: {}}");
    ASSERT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

TEST(AggregationRequestTest, ShouldIgnoreWriteConcernOption) {
    NamespaceString nss("a.collection");
    const BSONObj inputBson =
        fromjson("{pipeline: [{$match: {a: 'abc'}}], cursor: {}, writeConcern: 'invalid'}");
    ASSERT_OK(AggregationRequest::parseFromBSON(nss, inputBson).getStatus());
}

}  // namespace
}  // namespace mongo
