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

#include "mongo/db/views/resolved_view.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString viewNss = NamespaceString::createNamespaceString_forTest("testdb.testview");
const NamespaceString backingNss =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");
const std::vector<BSONObj> emptyPipeline;
const BSONObj kDefaultCursorOptionDocument = BSON(aggregation_request_helper::kBatchSizeField
                                                  << aggregation_request_helper::kDefaultBatchSize);
const BSONObj kSimpleCollation;

TEST(ResolvedViewTest, ExpandingAggRequestWithEmptyPipelineOnNoOpViewYieldsEmptyPipeline) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest requestOnView{viewNss, emptyPipeline};

    auto result = PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, requestOnView);
    BSONObj expected =
        BSON("aggregate" << backingNss.coll() << "pipeline" << BSONArray() << "cursor"
                         << kDefaultCursorOptionDocument << "collation" << BSONObj());
    ASSERT_BSONOBJ_EQ(result.toBSON(), expected);
}

TEST(ResolvedViewTest, ExpandingAggRequestWithNonemptyPipelineAppendsToViewPipeline) {
    std::vector<BSONObj> viewPipeline{BSON("skip" << 7)};
    const ResolvedView resolvedView{backingNss, viewPipeline, kSimpleCollation};
    AggregateCommandRequest requestOnView{viewNss, std::vector<BSONObj>{BSON("limit" << 3)}};

    auto result = PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, requestOnView);

    BSONObj expected =
        BSON("aggregate" << backingNss.coll() << "pipeline"
                         << BSON_ARRAY(BSON("skip" << 7) << BSON("limit" << 3)) << "cursor"
                         << kDefaultCursorOptionDocument << "collation" << BSONObj());
    ASSERT_BSONOBJ_EQ(result.toBSON(), expected);
}

TEST(ResolvedViewTest, ExpandingAggRequestWithCursorAndExplainOnlyPreservesExplain) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest{viewNss, std::vector<mongo::BSONObj>()};
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    aggRequest.setCursor(cursor);
    aggRequest.setExplain(true);

    auto result = PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, aggRequest);
    ASSERT(result.getExplain());
    ASSERT(*result.getExplain());
    ASSERT_EQ(
        result.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        aggregation_request_helper::kDefaultBatchSize);
}

TEST(ResolvedViewTest, ExpandingAggRequestWithCursorAndNoExplainPreservesCursor) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest{viewNss, std::vector<mongo::BSONObj>()};
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    aggRequest.setCursor(cursor);

    auto result = PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, aggRequest);
    ASSERT_FALSE(result.getExplain());
    ASSERT_EQ(
        result.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        10);
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesUnsetFields) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    auto aggRequest = AggregateCommandRequest(viewNss, std::vector<mongo::BSONObj>());

    // Set only a few fields on the request.
    aggRequest.setExplain(true);
    aggRequest.setLet(BSON("myVar" << 5));
    aggRequest.setNeedsMerge(true);
    aggRequest.setStmtId(123);
    aggRequest.setMaxTimeMS(100u);

    auto result = PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, aggRequest);

    // Verify that the namespace, pipeline, and collation were updated correctly.
    ASSERT_EQ(result.getNamespace(), backingNss);
    ASSERT_TRUE(result.getPipeline().empty());
    ASSERT(result.getCollation()->isEmpty());

    // Verify that the other set fields were preserved.
    ASSERT_TRUE(result.getExplain() && *result.getExplain());
    ASSERT_BSONOBJ_EQ(result.getLet().value(), BSON("myVar" << 5));
    ASSERT_TRUE(result.getNeedsMerge().value_or(false));
    ASSERT_EQ(result.getStmtId().value(), 123);
    ASSERT_EQ(result.getMaxTimeMS().value(), 100u);

    // Verify that all unset optional fields remain unset.
    ASSERT_FALSE(result.getAllowDiskUse().has_value());
    ASSERT_FALSE(result.getBypassDocumentValidation().has_value());
    ASSERT_FALSE(result.getHint().has_value());
    ASSERT_FALSE(result.getQuerySettings().has_value());
    ASSERT_FALSE(result.getFromMongos().has_value());
    ASSERT_FALSE(result.getNeedsSortedMerge().has_value());
    ASSERT_FALSE(result.getFromRouter().has_value());
    ASSERT_FALSE(result.getRequestReshardingResumeToken().has_value());
    ASSERT_FALSE(result.getIsMapReduceCommand().has_value());
    ASSERT_FALSE(result.getCollectionUUID().has_value());
    ASSERT_FALSE(result.getPassthroughToShard().has_value());
    ASSERT_FALSE(result.getEncryptionInformation().has_value());
    ASSERT_FALSE(result.getSampleId().has_value());
    ASSERT_FALSE(result.getIsClusterQueryWithoutShardKeyCmd().has_value());
    ASSERT_FALSE(result.getRequestResumeToken().has_value());
    ASSERT_FALSE(result.getResumeAfter().has_value());
    ASSERT_FALSE(result.getStartAt().has_value());
    ASSERT_FALSE(result.getIncludeQueryStatsMetrics().has_value());
    ASSERT_FALSE(result.getIsHybridSearch().has_value());
    ASSERT_FALSE(result.getReadConcern().has_value());
    ASSERT_FALSE(result.getUnwrappedReadPref().has_value());
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesMostFields) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    auto aggRequest = AggregateCommandRequest(viewNss, std::vector<mongo::BSONObj>());

    // Set all fields on the request.
    aggRequest.setExplain(true);
    aggRequest.setAllowDiskUse(true);
    aggRequest.setBypassDocumentValidation(true);
    aggRequest.setCollation(BSON("locale" << "en_US"));
    aggRequest.setHint(BSON("a" << 1));
    aggRequest.setQuerySettings(
        query_settings::QuerySettings::parse(BSON("queryFramework" << "classic")));
    aggRequest.setLet(BSON("myVar" << 5));
    aggRequest.setNeedsMerge(true);
    aggRequest.setNeedsSortedMerge(true);
    aggRequest.setFromMongos(true);
    aggRequest.setFromRouter(true);
    aggRequest.setRequestReshardingResumeToken(true);
    aggRequest.setExchange(ExchangeSpec::parse(BSON("policy" << "roundrobin"
                                                             << "consumers" << 3)));
    aggRequest.setIsMapReduceCommand(true);
    aggRequest.setCollectionUUID(UUID::gen());
    aggRequest.setPassthroughToShard(
        PassthroughToShardOptions::parse(BSON("shard" << "shard0000")));
    aggRequest.setEncryptionInformation(
        EncryptionInformation::parse(BSON("type" << 1 << "schema" << BSON("foo" << "bar"))));
    aggRequest.setSampleId(UUID::gen());
    aggRequest.setStmtId(123);
    aggRequest.setIsClusterQueryWithoutShardKeyCmd(true);
    aggRequest.setRequestResumeToken(true);
    aggRequest.setResumeAfter(BSON("rid" << 12345));
    aggRequest.setStartAt(BSON("rid" << 67890));
    aggRequest.setIncludeQueryStatsMetrics(true);
    aggRequest.setIsHybridSearch(true);
    aggRequest.setMaxTimeMS(100u);
    aggRequest.setReadConcern(repl::ReadConcernArgs::kLinearizable);
    aggRequest.setUnwrappedReadPref(BSON("$readPreference" << BSON("mode" << "secondary")));

    auto result = PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, aggRequest);

    // Verify that the namespace, pipeline, and collation were updated correctly.
    ASSERT_EQ(result.getNamespace(), backingNss);
    ASSERT_TRUE(result.getPipeline().empty());
    ASSERT(result.getCollation()->isEmpty());

    // Verify that all other fields were preserved.
    ASSERT_TRUE(result.getExplain() && *result.getExplain());
    ASSERT_TRUE(result.getAllowDiskUse());
    ASSERT_TRUE(result.getBypassDocumentValidation() && *result.getBypassDocumentValidation());
    ASSERT_BSONOBJ_EQ(result.getHint().value(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(result.getQuerySettings()->toBSON(), BSON("queryFramework" << "classic"));
    ASSERT_BSONOBJ_EQ(result.getLet().value(), BSON("myVar" << 5));
    ASSERT_TRUE(result.getNeedsMerge().value_or(false));
    ASSERT_TRUE(result.getNeedsSortedMerge().value_or(false));
    ASSERT_TRUE(result.getFromMongos().value_or(false));
    ASSERT_TRUE(result.getFromRouter().value_or(false));
    ASSERT_TRUE(result.getRequestReshardingResumeToken().value_or(false));
    ASSERT_BSONOBJ_EQ(result.getExchange()->toBSON(),
                      BSON("policy" << "roundrobin"
                                    << "consumers" << 3 << "orderPreserving" << false
                                    << "bufferSize" << 16777216 << "key" << BSONObj()));
    ASSERT_TRUE(result.getIsMapReduceCommand().value_or(false));
    ASSERT_EQ(result.getCollectionUUID(), aggRequest.getCollectionUUID());
    ASSERT_BSONOBJ_EQ(result.getPassthroughToShard()->toBSON(), BSON("shard" << "shard0000"));
    ASSERT_BSONOBJ_EQ(result.getEncryptionInformation()->toBSON(),
                      BSON("type" << 1 << "schema" << BSON("foo" << "bar")));
    ASSERT_EQ(result.getSampleId(), aggRequest.getSampleId());
    ASSERT_EQ(result.getStmtId().value(), 123);
    ASSERT_TRUE(result.getIsClusterQueryWithoutShardKeyCmd().value_or(false));
    ASSERT_TRUE(result.getRequestResumeToken().value_or(false));
    ASSERT_BSONOBJ_EQ(result.getResumeAfter().value(), BSON("rid" << 12345));
    ASSERT_BSONOBJ_EQ(result.getStartAt().value(), BSON("rid" << 67890));
    ASSERT_TRUE(result.getIncludeQueryStatsMetrics());
    ASSERT_TRUE(result.getIsHybridSearch().value_or(false));
    ASSERT_EQ(result.getMaxTimeMS().value(), 100u);
    ASSERT_BSONOBJ_EQ(result.getReadConcern()->toBSONInner(), BSON("level" << "linearizable"));
    ASSERT_BSONOBJ_EQ(result.getUnwrappedReadPref().value(),
                      BSON("$readPreference" << BSON("mode" << "secondary")));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesDefaultCollationOfView) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, BSON("locale" << "fr_CA")};
    ASSERT_BSONOBJ_EQ(resolvedView.getDefaultCollation(), BSON("locale" << "fr_CA"));
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());

    auto result = PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, aggRequest);
    ASSERT_BSONOBJ_EQ(result.getCollation().value_or(BSONObj()), BSON("locale" << "fr_CA"));
}

TEST(ResolvedViewTest, EnsureSerializationContextCopy) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};

    AggregateCommandRequest requestOnViewDefault{viewNss, emptyPipeline};

    auto resultDefault =
        PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, requestOnViewDefault);
    ASSERT_TRUE(resultDefault.getSerializationContext() ==
                SerializationContext::stateCommandRequest());

    SerializationContext scCommand = SerializationContext::stateCommandRequest();
    scCommand.setPrefixState(true);
    AggregateCommandRequest requestOnViewCommand{viewNss, emptyPipeline, scCommand};

    auto resultCommand =
        PipelineResolver::buildRequestWithResolvedPipeline(resolvedView, requestOnViewCommand);
    ASSERT_EQ(resultCommand.getSerializationContext().getSource(),
              SerializationContext::Source::Command);
    ASSERT_EQ(resultCommand.getSerializationContext().getCallerType(),
              SerializationContext::CallerType::Request);
    ASSERT_EQ(resultCommand.getSerializationContext().getPrefix(),
              SerializationContext::Prefix::IncludePrefix);
}

TEST(ResolvedViewTest, FromBSONFailsIfMissingResolvedView) {
    BSONObj badCmdResponse = BSON("x" << 1);
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40248);
}

TEST(ResolvedViewTest, FromBSONFailsOnResolvedViewBadType) {
    BSONObj badCmdResponse = BSON("resolvedView" << 7);
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40249);
}

TEST(ResolvedViewTest, FromBSONFailsIfMissingViewNs) {
    BSONObj badCmdResponse = BSON("resolvedView" << BSON("pipeline" << BSONArray()));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40250);
}

TEST(ResolvedViewTest, FromBSONFailsOnInvalidViewNsType) {
    BSONObj badCmdResponse = BSON("resolvedView" << BSON("ns" << 8));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40250);
}

TEST(ResolvedViewTest, FromBSONFailsIfMissingPipeline) {
    BSONObj badCmdResponse = BSON("resolvedView" << BSON("ns" << backingNss.ns_forTest()));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40251);
}

TEST(ResolvedViewTest, FromBSONFailsOnInvalidPipelineType) {
    BSONObj badCmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns_forTest() << "pipeline" << 7));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40251);
}

TEST(ResolvedViewTest, FromBSONFailsOnInvalidCollationType) {
    BSONObj badCmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns_forTest() << "pipeline" << BSONArray()
                                         << "collation" << 1));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40639);
}

TEST(ResolvedViewTest, FromBSONSuccessfullyParsesEmptyBSONArrayIntoEmptyVector) {
    BSONObj cmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns_forTest() << "pipeline" << BSONArray()));
    const ResolvedView result = ResolvedView::fromBSON(cmdResponse);
    ASSERT_EQ(result.getNamespace(), backingNss);
    ASSERT(std::equal(emptyPipeline.begin(),
                      emptyPipeline.end(),
                      result.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(ResolvedViewTest, FromBSONSuccessfullyParsesCollation) {
    BSONObj cmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns_forTest() << "pipeline" << BSONArray()
                                         << "collation" << BSON("locale" << "fil")));
    const ResolvedView result = ResolvedView::fromBSON(cmdResponse);
    ASSERT_EQ(result.getNamespace(), backingNss);
    ASSERT(std::equal(emptyPipeline.begin(),
                      emptyPipeline.end(),
                      result.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT_BSONOBJ_EQ(result.getDefaultCollation(), BSON("locale" << "fil"));
}

TEST(ResolvedViewTest, FromBSONSuccessfullyParsesPopulatedBSONArrayIntoVector) {
    BSONObj matchStage = BSON("$match" << BSON("x" << 1));
    BSONObj sortStage = BSON("$sort" << BSON("y" << -1));
    BSONObj limitStage = BSON("$limit" << 7);

    BSONArray pipeline = BSON_ARRAY(matchStage << sortStage << limitStage);
    BSONObj cmdResponse = BSON("resolvedView" << BSON("ns" << "testdb.testcoll"
                                                           << "pipeline" << pipeline));

    const ResolvedView result = ResolvedView::fromBSON(cmdResponse);
    ASSERT_EQ(result.getNamespace(), backingNss);

    std::vector<BSONObj> expectedPipeline{matchStage, sortStage, limitStage};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      result.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(ResolvedViewTest, IsResolvedViewErrorResponseDetectsKickbackErrorCodeSuccessfully) {
    BSONObj errorResponse =
        BSON("ok" << 0 << "code" << ErrorCodes::CommandOnShardedViewNotSupportedOnMongod << "errmsg"
                  << "This view is sharded and cannot be run on mongod"
                  << "resolvedView"
                  << BSON("ns" << backingNss.ns_forTest() << "pipeline" << BSONArray()));
    auto status = getStatusFromCommandResult(errorResponse);
    ASSERT_EQ(status, ErrorCodes::CommandOnShardedViewNotSupportedOnMongod);
    ASSERT(status.extraInfo<ResolvedView>());
}

TEST(ResolvedViewTest, IsResolvedViewErrorResponseReportsFalseOnNonKickbackErrorCode) {
    BSONObj errorResponse =
        BSON("ok" << 0 << "code" << ErrorCodes::ViewDepthLimitExceeded << "errmsg"
                  << "View nesting too deep or view cycle detected");
    auto status = getStatusFromCommandResult(errorResponse);
    ASSERT_NE(status, ErrorCodes::CommandOnShardedViewNotSupportedOnMongod);
    ASSERT(!status.extraInfo<ResolvedView>());
}

TEST(ResolvedViewTest, SerializesCorrectly) {
    const ResolvedView resolvedView{backingNss,
                                    std::vector<BSONObj>{BSON("$match" << BSON("x" << 1))},
                                    BSON("locale" << "fr_CA")};
    BSONObjBuilder bob;
    resolvedView.serialize(&bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), fromjson(R"({
        resolvedView: {
            ns: 'testdb.testcoll',
            pipeline: [{$match: {x: 1}}],
            collation: {locale: 'fr_CA'}
        }
    })"));
}

TEST(ResolvedViewTest, SerializeOutputCanBeReparsed) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, BSON("locale" << "fr_CA")};
    BSONObjBuilder bob;
    resolvedView.serialize(&bob);
    auto reparsedResolvedView = ResolvedView::fromBSON(bob.obj());
    ASSERT_EQ(reparsedResolvedView.getNamespace(), backingNss);
    ASSERT(std::equal(emptyPipeline.begin(),
                      emptyPipeline.end(),
                      reparsedResolvedView.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT_BSONOBJ_EQ(reparsedResolvedView.getDefaultCollation(), BSON("locale" << "fr_CA"));
}

TEST(ResolvedViewTest, ParseFromBSONCorrectly) {
    BSONObj searchStage = BSON("$search" << BSON("text" << "foo"));
    BSONObj matchStage = BSON("$match" << BSON("x" << 1));
    BSONArray pipeline = BSON_ARRAY(searchStage << matchStage);

    BSONObj cmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns_forTest() << "pipeline" << pipeline
                                         << "collation" << BSON("locale" << "fil")));
    BSONElement elem = cmdResponse.getField("resolvedView");

    std::vector<BSONObj> expectedPipeline{searchStage, matchStage};
    const ResolvedView result = ResolvedView::parseFromBSON(elem);
    ASSERT_EQ(result.getNamespace(), backingNss);
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      result.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT_BSONOBJ_EQ(result.getDefaultCollation(), BSON("locale" << "fil"));
}

TEST(ResolvedViewTest, ParseFromBSONFailsIfNotAnObject) {
    BSONObj cmdResponse = BSON("resolvedView" << "ThisIsNotAnObject");
    BSONElement elem = cmdResponse.getField("resolvedView");

    ASSERT_THROWS_CODE(ResolvedView::parseFromBSON(elem), AssertionException, 936370);
}

TEST(ResolvedViewTest, ParseFromBSONFailsIfEmptyObject) {
    BSONObj cmdResponse = BSON("resolvedView" << BSONObj());
    BSONElement elem = cmdResponse.getField("resolvedView");

    ASSERT_THROWS_CODE(ResolvedView::parseFromBSON(elem), AssertionException, 40249);
}

TEST(ResolvedViewTest, SerializeToBSONCorrectly) {
    const ResolvedView resolvedView{backingNss,
                                    std::vector<BSONObj>{BSON("$search" << BSON("text" << "foo")),
                                                         BSON("$match" << BSON("x" << 1))},
                                    BSON("locale" << "fil")};
    BSONObjBuilder bob;
    resolvedView.serializeToBSON("resolvedView", &bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), fromjson(R"({
        resolvedView: {
            ns: 'testdb.testcoll',
            pipeline: [{$search: {text: "foo"}}, {$match: {x: 1}}],
            collation: {locale: 'fil'}
        }
    })"));
}

TEST(ResolvedViewTest, IDLParserRoundtrip) {
    BSONObj searchStage = BSON("$search" << BSON("text" << "foo"));
    BSONObj matchStage = BSON("$match" << BSON("x" << 1));
    BSONArray pipeline = BSON_ARRAY(searchStage << matchStage);

    BSONObj cmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns_forTest() << "pipeline" << pipeline
                                         << "collation" << BSON("locale" << "fil")));
    BSONElement elem = cmdResponse.getField("resolvedView");
    const ResolvedView fromObj = ResolvedView::parseFromBSON(elem);

    BSONObjBuilder toObj;
    fromObj.serializeToBSON("resolvedView", &toObj);
    ASSERT_BSONOBJ_EQ(toObj.obj(), fromjson(R"({
        resolvedView: {
            ns: 'testdb.testcoll',
            pipeline: [{$search: {text: "foo"}}, {$match: {x: 1}}],
            collation: {locale: 'fil'}
        }
    })"));
}
}  // namespace
}  // namespace mongo
