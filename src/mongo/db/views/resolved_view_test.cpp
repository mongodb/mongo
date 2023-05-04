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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"

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

    auto result = resolvedView.asExpandedViewAggregation(requestOnView);
    BSONObj expected =
        BSON("aggregate" << backingNss.coll() << "pipeline" << BSONArray() << "cursor"
                         << kDefaultCursorOptionDocument << "collation" << BSONObj());
    ASSERT_BSONOBJ_EQ(aggregation_request_helper::serializeToCommandObj(result), expected);
}

TEST(ResolvedViewTest, ExpandingAggRequestWithNonemptyPipelineAppendsToViewPipeline) {
    std::vector<BSONObj> viewPipeline{BSON("skip" << 7)};
    const ResolvedView resolvedView{backingNss, viewPipeline, kSimpleCollation};
    AggregateCommandRequest requestOnView{viewNss, std::vector<BSONObj>{BSON("limit" << 3)}};

    auto result = resolvedView.asExpandedViewAggregation(requestOnView);

    BSONObj expected =
        BSON("aggregate" << backingNss.coll() << "pipeline"
                         << BSON_ARRAY(BSON("skip" << 7) << BSON("limit" << 3)) << "cursor"
                         << kDefaultCursorOptionDocument << "collation" << BSONObj());
    ASSERT_BSONOBJ_EQ(aggregation_request_helper::serializeToCommandObj(result), expected);
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesExplain) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest{viewNss, std::vector<mongo::BSONObj>()};
    aggRequest.setExplain(ExplainOptions::Verbosity::kExecStats);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT(result.getExplain());
    ASSERT(*result.getExplain() == ExplainOptions::Verbosity::kExecStats);
}

TEST(ResolvedViewTest, ExpandingAggRequestWithCursorAndExplainOnlyPreservesExplain) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest{viewNss, std::vector<mongo::BSONObj>()};
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    aggRequest.setCursor(cursor);
    aggRequest.setExplain(ExplainOptions::Verbosity::kExecStats);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT(result.getExplain());
    ASSERT(*result.getExplain() == ExplainOptions::Verbosity::kExecStats);
    ASSERT_EQ(
        result.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        aggregation_request_helper::kDefaultBatchSize);
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesBypassDocumentValidation) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());
    aggRequest.setBypassDocumentValidation(true);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_TRUE(result.getBypassDocumentValidation().value_or(false));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesAllowDiskUse) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());
    aggRequest.setAllowDiskUse(true);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_TRUE(result.getAllowDiskUse());
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesHint) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());
    aggRequest.setHint(BSON("a" << 1));

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getHint().value_or(BSONObj()), BSON("a" << 1));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesReadPreference) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());
    aggRequest.setUnwrappedReadPref(BSON("$readPreference"
                                         << "nearest"));

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getUnwrappedReadPref().value_or(BSONObj()),
                      BSON("$readPreference"
                           << "nearest"));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesReadConcern) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());
    aggRequest.setReadConcern(BSON("level"
                                   << "linearizable"));

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getReadConcern().value_or(BSONObj()),
                      BSON("level"
                           << "linearizable"));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesMaxTimeMS) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());
    aggRequest.setMaxTimeMS(100u);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_EQ(result.getMaxTimeMS().value_or(0), 100u);
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesDefaultCollationOfView) {
    const ResolvedView resolvedView{backingNss,
                                    emptyPipeline,
                                    BSON("locale"
                                         << "fr_CA")};
    ASSERT_BSONOBJ_EQ(resolvedView.getDefaultCollation(),
                      BSON("locale"
                           << "fr_CA"));
    AggregateCommandRequest aggRequest(viewNss, std::vector<mongo::BSONObj>());

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getCollation().value_or(BSONObj()),
                      BSON("locale"
                           << "fr_CA"));
}

TEST(ResolvedViewTest, EnsureSerializationContextCopy) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};

    AggregateCommandRequest requestOnViewDefault{viewNss, emptyPipeline};

    auto resultDefault = resolvedView.asExpandedViewAggregation(requestOnViewDefault);
    ASSERT_TRUE(resultDefault.getSerializationContext() ==
                SerializationContext::stateCommandRequest());

    SerializationContext scCommand = SerializationContext::stateCommandRequest();
    scCommand.setTenantIdSource(true);
    scCommand.setPrefixState(true);
    AggregateCommandRequest requestOnViewCommand{viewNss, emptyPipeline, scCommand};

    auto resultCommand = resolvedView.asExpandedViewAggregation(requestOnViewCommand);
    ASSERT_EQ(resultCommand.getSerializationContext().getSource(),
              SerializationContext::Source::Command);
    ASSERT_EQ(resultCommand.getSerializationContext().getCallerType(),
              SerializationContext::CallerType::Request);
    ASSERT_TRUE(resultCommand.getSerializationContext().receivedNonPrefixedTenantId());
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
    BSONObj badCmdResponse = BSON("resolvedView" << BSON("ns" << backingNss.ns()));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40251);
}

TEST(ResolvedViewTest, FromBSONFailsOnInvalidPipelineType) {
    BSONObj badCmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns() << "pipeline" << 7));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40251);
}

TEST(ResolvedViewTest, FromBSONFailsOnInvalidCollationType) {
    BSONObj badCmdResponse = BSON("resolvedView" << BSON("ns" << backingNss.ns() << "pipeline"
                                                              << BSONArray() << "collation" << 1));
    ASSERT_THROWS_CODE(ResolvedView::fromBSON(badCmdResponse), AssertionException, 40639);
}

TEST(ResolvedViewTest, FromBSONSuccessfullyParsesEmptyBSONArrayIntoEmptyVector) {
    BSONObj cmdResponse =
        BSON("resolvedView" << BSON("ns" << backingNss.ns() << "pipeline" << BSONArray()));
    const ResolvedView result = ResolvedView::fromBSON(cmdResponse);
    ASSERT_EQ(result.getNamespace(), backingNss);
    ASSERT(std::equal(emptyPipeline.begin(),
                      emptyPipeline.end(),
                      result.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(ResolvedViewTest, FromBSONSuccessfullyParsesCollation) {
    BSONObj cmdResponse = BSON("resolvedView" << BSON("ns" << backingNss.ns() << "pipeline"
                                                           << BSONArray() << "collation"
                                                           << BSON("locale"
                                                                   << "fil")));
    const ResolvedView result = ResolvedView::fromBSON(cmdResponse);
    ASSERT_EQ(result.getNamespace(), backingNss);
    ASSERT(std::equal(emptyPipeline.begin(),
                      emptyPipeline.end(),
                      result.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT_BSONOBJ_EQ(result.getDefaultCollation(),
                      BSON("locale"
                           << "fil"));
}

TEST(ResolvedViewTest, FromBSONSuccessfullyParsesPopulatedBSONArrayIntoVector) {
    BSONObj matchStage = BSON("$match" << BSON("x" << 1));
    BSONObj sortStage = BSON("$sort" << BSON("y" << -1));
    BSONObj limitStage = BSON("$limit" << 7);

    BSONArray pipeline = BSON_ARRAY(matchStage << sortStage << limitStage);
    BSONObj cmdResponse = BSON("resolvedView" << BSON("ns"
                                                      << "testdb.testcoll"
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
                  << "resolvedView" << BSON("ns" << backingNss.ns() << "pipeline" << BSONArray()));
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
                                    BSON("locale"
                                         << "fr_CA")};
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
    const ResolvedView resolvedView{backingNss,
                                    emptyPipeline,
                                    BSON("locale"
                                         << "fr_CA")};
    BSONObjBuilder bob;
    resolvedView.serialize(&bob);
    auto reparsedResolvedView = ResolvedView::fromBSON(bob.obj());
    ASSERT_EQ(reparsedResolvedView.getNamespace(), backingNss);
    ASSERT(std::equal(emptyPipeline.begin(),
                      emptyPipeline.end(),
                      reparsedResolvedView.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT_BSONOBJ_EQ(reparsedResolvedView.getDefaultCollation(),
                      BSON("locale"
                           << "fr_CA"));
}
}  // namespace
}  // namespace mongo
