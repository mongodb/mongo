/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString viewNss("testdb.testview");
const NamespaceString backingNss("testdb.testcoll");
const std::vector<BSONObj> emptyPipeline;
const BSONObj kDefaultCursorOptionDocument =
    BSON(AggregationRequest::kBatchSizeName << AggregationRequest::kDefaultBatchSize);
const BSONObj kSimpleCollation;

TEST(ResolvedViewTest, ExpandingAggRequestWithEmptyPipelineOnNoOpViewYieldsEmptyPipeline) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest requestOnView{viewNss, emptyPipeline};

    auto result = resolvedView.asExpandedViewAggregation(requestOnView);
    BSONObj expected =
        BSON("aggregate" << backingNss.coll() << "pipeline" << BSONArray() << "cursor"
                         << kDefaultCursorOptionDocument);
    ASSERT_BSONOBJ_EQ(result.serializeToCommandObj().toBson(), expected);
}

TEST(ResolvedViewTest, ExpandingAggRequestWithNonemptyPipelineAppendsToViewPipeline) {
    std::vector<BSONObj> viewPipeline{BSON("skip" << 7)};
    const ResolvedView resolvedView{backingNss, viewPipeline, kSimpleCollation};
    AggregationRequest requestOnView{viewNss, std::vector<BSONObj>{BSON("limit" << 3)}};

    auto result = resolvedView.asExpandedViewAggregation(requestOnView);

    BSONObj expected = BSON("aggregate" << backingNss.coll() << "pipeline"
                                        << BSON_ARRAY(BSON("skip" << 7) << BSON("limit" << 3))
                                        << "cursor"
                                        << kDefaultCursorOptionDocument);
    ASSERT_BSONOBJ_EQ(result.serializeToCommandObj().toBson(), expected);
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesExplain) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest{viewNss, {}};
    aggRequest.setExplain(ExplainOptions::Verbosity::kExecStats);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT(result.getExplain());
    ASSERT(*result.getExplain() == ExplainOptions::Verbosity::kExecStats);
}

TEST(ResolvedViewTest, ExpandingAggRequestWithCursorAndExplainOnlyPreservesExplain) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest{viewNss, {}};
    aggRequest.setBatchSize(10);
    aggRequest.setExplain(ExplainOptions::Verbosity::kExecStats);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT(result.getExplain());
    ASSERT(*result.getExplain() == ExplainOptions::Verbosity::kExecStats);
    ASSERT_EQ(result.getBatchSize(), AggregationRequest::kDefaultBatchSize);
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesBypassDocumentValidation) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest(viewNss, {});
    aggRequest.setBypassDocumentValidation(true);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_TRUE(result.shouldBypassDocumentValidation());
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesAllowDiskUse) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest(viewNss, {});
    aggRequest.setAllowDiskUse(true);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_TRUE(result.shouldAllowDiskUse());
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesHint) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest(viewNss, {});
    aggRequest.setHint(BSON("a" << 1));

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getHint(), BSON("a" << 1));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesReadPreference) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest(viewNss, {});
    aggRequest.setUnwrappedReadPref(BSON("$readPreference"
                                         << "nearest"));

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getUnwrappedReadPref(),
                      BSON("$readPreference"
                           << "nearest"));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesReadConcern) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest(viewNss, {});
    aggRequest.setReadConcern(BSON("level"
                                   << "linearizable"));

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getReadConcern(),
                      BSON("level"
                           << "linearizable"));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesMaxTimeMS) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest(viewNss, {});
    aggRequest.setMaxTimeMS(100u);

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_EQ(result.getMaxTimeMS(), 100u);
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesDefaultCollationOfView) {
    const ResolvedView resolvedView{backingNss,
                                    emptyPipeline,
                                    BSON("locale"
                                         << "fr_CA")};
    ASSERT_BSONOBJ_EQ(resolvedView.getDefaultCollation(),
                      BSON("locale"
                           << "fr_CA"));
    AggregationRequest aggRequest(viewNss, {});

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_BSONOBJ_EQ(result.getCollation(),
                      BSON("locale"
                           << "fr_CA"));
}

TEST(ResolvedViewTest, ExpandingAggRequestPreservesComment) {
    const ResolvedView resolvedView{backingNss, emptyPipeline, kSimpleCollation};
    AggregationRequest aggRequest(viewNss, {});
    aggRequest.setComment("agg_comment");

    auto result = resolvedView.asExpandedViewAggregation(aggRequest);
    ASSERT_EQ(result.getComment(), "agg_comment");
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
    BSONObj badCmdResponse =
        BSON("resolvedView" << BSON(
                 "ns" << backingNss.ns() << "pipeline" << BSONArray() << "collation" << 1));
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
    BSONObj cmdResponse = BSON(
        "resolvedView" << BSON("ns" << backingNss.ns() << "pipeline" << BSONArray() << "collation"
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
                                                      << "pipeline"
                                                      << pipeline));

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
                  << "This view is sharded and cannot be run on mongod");
    ASSERT(ResolvedView::isResolvedViewErrorResponse(errorResponse));
}

TEST(ResolvedViewTest, IsResolvedViewErrorResponseReportsFalseOnNonKickbackErrorCode) {
    BSONObj errorResponse =
        BSON("ok" << 0 << "code" << ErrorCodes::ViewDepthLimitExceeded << "errmsg"
                  << "View nesting too deep or view cycle detected");
    ASSERT_FALSE(ResolvedView::isResolvedViewErrorResponse(errorResponse));
}

TEST(ResolvedViewTest, ToBSONSerializesCorrectly) {
    const ResolvedView resolvedView{backingNss,
                                    std::vector<BSONObj>{BSON("$match" << BSON("x" << 1))},
                                    BSON("locale"
                                         << "fr_CA")};
    auto serialized = resolvedView.toBSON();
    ASSERT_BSONOBJ_EQ(
        serialized,
        fromjson(
            "{ns: 'testdb.testcoll', pipeline: [{$match: {x: 1}}], collation: {locale: 'fr_CA'}}"));
}

TEST(ResolvedViewTest, ToBSONOutputCanBeReparsed) {
    const ResolvedView resolvedView{backingNss,
                                    emptyPipeline,
                                    BSON("locale"
                                         << "fr_CA")};
    auto serialized = resolvedView.toBSON();
    auto reparsedResolvedView = ResolvedView::fromBSON(BSON("resolvedView" << serialized));
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
