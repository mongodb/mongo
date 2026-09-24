// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/router_stage_pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {
namespace {

using RouterStagePipelineTest = AggregationContextFixture;
using RouterStagePipelineDeathTest = RouterStagePipelineTest;

// Builds a change stream event whose resume token is derived from 'clusterTime'. As the change
// stream event transform does, the resume token is installed both as the document's '_id' field and
// as its sort key metadata.
Document makeChangeStreamEvent(Timestamp clusterTime) {
    ResumeTokenData tokenData{clusterTime,
                              ResumeTokenData::kDefaultTokenVersion,
                              0, /* txnOpIndex */
                              UUID::gen(),
                              Value(Document(BSON("operationType" << "insert")))};
    auto tokenBson = ResumeToken(tokenData).toDocument().toBson();

    MutableDocument doc;
    doc.addField("_id", Value(Document(tokenBson)));
    doc.metadata().setSortKey(Value(Document(tokenBson)), true);
    return doc.freeze();
}

std::unique_ptr<RouterStagePipeline> makeRouterStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, std::vector<Document> events) {
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    DocumentSourceContainer sources;
    sources.push_back(DocumentSourceMock::createForTest(events, expCtx));
    return std::make_unique<RouterStagePipeline>(Pipeline::create(sources, expCtx));
}

TEST_F(RouterStagePipelineTest, AllowsMonotonicallyIncreasingResumeTokens) {
    auto stage = makeRouterStage(
        getExpCtx(),
        {makeChangeStreamEvent(Timestamp(1, 1)), makeChangeStreamEvent(Timestamp(2, 1))});

    ASSERT_OK(stage->next().getStatus());
    ASSERT_OK(stage->next().getStatus());
    ASSERT_OK(stage->next().getStatus());
    ASSERT(stage->next().getValue().isEOF());
}

DEATH_TEST_REGEX_F(RouterStagePipelineDeathTest,
                   RejectsResumeTokenWhichGoesBackwards,
                   "Tripwire assertion.*13536500") {
    auto stage = makeRouterStage(
        getExpCtx(),
        {makeChangeStreamEvent(Timestamp(2, 1)), makeChangeStreamEvent(Timestamp(1, 1))});

    ASSERT_OK(stage->next().getStatus());
    ASSERT_THROWS_CODE(stage->next(), AssertionException, 13536500);
}

TEST_F(RouterStagePipelineTest, DoesNotRejectRepeatedResumeToken) {
    auto event = makeChangeStreamEvent(Timestamp(1, 1));
    auto stage = makeRouterStage(getExpCtx(), {event, event});

    ASSERT_OK(stage->next().getStatus());
    ASSERT_OK(stage->next().getStatus());
}

}  // namespace
}  // namespace mongo
