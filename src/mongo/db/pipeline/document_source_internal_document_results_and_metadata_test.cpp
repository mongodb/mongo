/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/exchange_stage.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_mock_stages.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr std::string_view kStageName = "$_internalDocumentResultsAndMetadata"sv;

const auto kSourceOnly =
    BSON("source" << BSON("$collStats" << BSONObj()) << "returnCursor" << false);
const auto kSourceWithMeta =
    BSON("source" << BSON("$collStats" << BSONObj()) << "metadata" << BSON("as" << "SEARCH_META")
                  << "returnCursor" << false);
const auto kFullSpec = BSON("source" << BSON("$collStats" << BSONObj()) << "metadata"
                                     << BSON("as" << "SEARCH_META") << "returnCursor" << true);

class DocumentSourceInternalDocumentResultsAndMetadataTest : public AggregationContextFixture {
protected:
    DocumentSourceInternalDocumentResultsAndMetadata* parse(const BSONObj& bson) {
        auto specElem = bson.firstElement();
        // Route through the LiteParsed layer so the StageParams carry the inner lite-parsed
        // sub-pipeline — same shape as production. makeOwned() so that callers can pass a
        // temporary BSONObj without dangling the LP's BSONElement view.
        _liteParsed = InternalDocumentResultsAndMetadataLiteParsed::parse(
            getExpCtx()->getNamespaceString(), specElem, {});
        _liteParsed->makeOwned();
        auto params = _liteParsed->getStageParams();
        auto* typedParams =
            dynamic_cast<InternalDocumentResultsAndMetadataStageParams*>(params.get());
        ASSERT(typedParams);
        _stages = DocumentSourceInternalDocumentResultsAndMetadata::createFromStageParams(
            *typedParams, getExpCtx());
        ASSERT_EQ(_stages.size(), 1u);
        auto* docResultsAndMetadata =
            dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(_stages.front().get());
        ASSERT(docResultsAndMetadata);
        return docResultsAndMetadata;
    }

private:
    std::unique_ptr<InternalDocumentResultsAndMetadataLiteParsed> _liteParsed;
    DocumentSourceContainer _stages;
};
using DocumentSourceInternalDocumentResultsAndMetadataDeathTest =
    DocumentSourceInternalDocumentResultsAndMetadataTest;

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, ParsesFullSpec) {
    auto* stage = parse(BSON(kStageName << kFullSpec));
    ASSERT_EQ(stage->getSourceStage()->getSourceName(), std::string_view("$collStats"));
    ASSERT(stage->getMetadata().has_value());
    ASSERT_EQ(stage->getMetadata()->getAs(), "SEARCH_META");
    ASSERT_TRUE(stage->getReturnCursor());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, ParsesSpecWithoutMetadata) {
    auto* stage = parse(BSON(kStageName << kSourceOnly));
    ASSERT_FALSE(stage->getMetadata().has_value());
    ASSERT_FALSE(stage->getReturnCursor());
}

DEATH_TEST_F(DocumentSourceInternalDocumentResultsAndMetadataDeathTest,
             RejectsSourceThatRequiresInputDocSource,
             "12615004") {
    parse(BSON(kStageName << BSON("source" << BSON("$limit" << 1))));
}

DEATH_TEST_F(DocumentSourceInternalDocumentResultsAndMetadataDeathTest,
             RejectsUnknownMetadataVariableName,
             "12615005") {
    parse(BSON(kStageName << BSON("source" << BSON("$collStats" << BSONObj()) << "metadata"
                                           << BSON("as" << "UNKNOWN_VARIABLE"))));
}

DEATH_TEST_F(DocumentSourceInternalDocumentResultsAndMetadataDeathTest,
             RejectsVariableNameWhenNotSearchMeta,
             "12615005") {
    parse(BSON(kStageName << BSON("source" << BSON("$collStats" << BSONObj()) << "metadata"
                                           << BSON("as" << "NOW"))));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, RejectsMissingSource) {
    ASSERT_THROWS_CODE(parse(BSON(kStageName << BSON("metadata" << BSON("as" << "SEARCH_META")))),
                       AssertionException,
                       40414);
}

// Serialize tests
TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, SerializesFullSpec) {
    auto* stage = parse(BSON(kStageName << kFullSpec));

    auto serialized = stage->serialize(query_shape::SerializationOptions{});
    ASSERT_BSONOBJ_EQ(serialized.getDocument().toBson(), BSON(kStageName << kFullSpec));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, SerializesWithoutMetadata) {
    auto* stage = parse(BSON(kStageName << kSourceOnly));

    auto serialized = stage->serialize(query_shape::SerializationOptions{});
    ASSERT_BSONOBJ_EQ(serialized.getDocument().toBson(), BSON(kStageName << kSourceOnly));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       SerializeIncludesReturnCursorWhenFalse) {
    auto* stage = parse(BSON(kStageName << kSourceWithMeta));

    auto serialized = stage->serialize(query_shape::SerializationOptions{});
    auto bson = serialized.getDocument().toBson();
    auto inner = bson.getObjectField(kStageName);
    ASSERT_FALSE(inner["returnCursor"].Bool());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, SerializesWithIdentifierTransform) {
    auto* stage = parse(BSON(kStageName << kSourceWithMeta));

    auto serialized =
        stage->serialize(query_shape::SerializationOptions::kMarkIdentifiers_FOR_TEST);
    auto bson = serialized.getDocument().toBson();
    auto inner = bson.getObjectField(kStageName);

    ASSERT_EQ(inner.getObjectField("metadata").getStringField("as"), "HASH<SEARCH_META>");
}

// Stage Constraints tests.
TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, ConstraintsDerivedFromCollStats) {
    auto* stage = parse(BSON(kStageName << kSourceWithMeta));

    auto containerConstraints = stage->constraints(PipelineSplitState::kUnsplit);
    auto sourceConstraints = stage->getSourceStage()->constraints(PipelineSplitState::kUnsplit);

    ASSERT(containerConstraints == sourceConstraints);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, ConstraintsDerivedFromIndexStats) {
    auto* stage = parse(BSON(kStageName << BSON("source" << BSON("$indexStats" << BSONObj()))));

    auto containerConstraints = stage->constraints(PipelineSplitState::kUnsplit);
    auto sourceConstraints = stage->getSourceStage()->constraints(PipelineSplitState::kUnsplit);

    ASSERT(containerConstraints == sourceConstraints);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, ConstraintsRequiresFirstPosition) {
    auto* stage = parse(BSON(kStageName << kSourceOnly));

    auto constraints = stage->constraints(PipelineSplitState::kUnsplit);

    ASSERT_EQ(constraints.requiredPosition, StageConstraints::PositionRequirement::kFirst);
    ASSERT_FALSE(constraints.requiresInputDocSource);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       ConstraintsOverridesFacetAndTransaction) {
    auto mock = DocumentSourceMock::createForTest(Document{}, getExpCtx());
    mock->mockConstraints.requiredPosition = StageConstraints::PositionRequirement::kFirst;
    mock->mockConstraints.facetRequirement = StageConstraints::FacetRequirement::kAllowed;
    mock->mockConstraints.transactionRequirement =
        StageConstraints::TransactionRequirement::kAllowed;

    auto stage = DocumentSourceInternalDocumentResultsAndMetadata::create(
        getExpCtx(), std::move(mock), boost::none);

    auto constraints = stage->constraints(PipelineSplitState::kUnsplit);
    ASSERT_EQ(constraints.facetRequirement, StageConstraints::FacetRequirement::kNotAllowed);
    ASSERT_EQ(constraints.transactionRequirement,
              StageConstraints::TransactionRequirement::kNotAllowed);
}

class DocumentSourceMockSkipMetadataStream final : public DocumentSourceMock {
public:
    static boost::intrusive_ptr<DocumentSourceMockSkipMetadataStream> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        boost::intrusive_ptr<DocumentSourceMockSkipMetadataStream> mock(
            new DocumentSourceMockSkipMetadataStream(expCtx));
        mock->mockConstraints.requiredPosition = StageConstraints::PositionRequirement::kFirst;
        mock->mockConstraints.requiresInputDocSource = false;
        return mock;
    }

    void skipMetadataStream() override {
        _metadataStreamSkipped = true;
    }

    bool isMetadataStreamSkipped() const {
        return _metadataStreamSkipped;
    }

private:
    DocumentSourceMockSkipMetadataStream(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    bool _metadataStreamSkipped = false;
};

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       OptimizeElidesMetadataWhenNoSearchMetaRef) {
    auto sourceStage = DocumentSourceMockSkipMetadataStream::create(getExpCtx());
    auto* sourcePtr = sourceStage.get();
    auto stage = DocumentSourceInternalDocumentResultsAndMetadata::create(
        getExpCtx(), std::move(sourceStage), MetadataBindSpec("SEARCH_META"));
    auto* ds = dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stage.get());
    ASSERT(ds);
    ASSERT(ds->getMetadata().has_value());

    // Downstream $project does not reference $$SEARCH_META.
    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    DocumentSourceContainer pipeline;
    pipeline.push_back(stage);
    pipeline.push_back(downstreamStage);
    ds->optimizeAt(pipeline.begin(), &pipeline);

    ASSERT_FALSE(ds->getMetadata().has_value());
    ASSERT_TRUE(sourcePtr->isMetadataStreamSkipped());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       OptimizeRetainsMetadataWhenNeedsMergeTrue) {
    // Simulate shard context: needsMerge = true.
    getExpCtx()->setNeedsMerge(true);
    auto sourceStage = DocumentSource::parse(getExpCtx(), BSON("$collStats" << BSONObj())).front();
    auto stage = DocumentSourceInternalDocumentResultsAndMetadata::create(
        getExpCtx(), std::move(sourceStage), MetadataBindSpec("SEARCH_META"));
    auto* ds = dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stage.get());
    ASSERT(ds);

    // Downstream $project does not reference $$SEARCH_META but elision must be suppressed on
    // shards.
    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    DocumentSourceContainer pipeline;
    pipeline.push_back(stage);
    pipeline.push_back(downstreamStage);
    ds->optimizeAt(pipeline.begin(), &pipeline);

    ASSERT_TRUE(ds->getMetadata().has_value());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       OptimizeSkipsMetadataStreamWhenMetadataAlreadyAbsent) {
    auto sourceStage = DocumentSourceMockSkipMetadataStream::create(getExpCtx());
    auto* sourcePtr = sourceStage.get();
    auto stage = DocumentSourceInternalDocumentResultsAndMetadata::create(
        getExpCtx(), std::move(sourceStage), /*metadata=*/boost::none);
    auto* ds = dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stage.get());
    ASSERT(ds);
    ASSERT_FALSE(ds->getMetadata().has_value());

    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    DocumentSourceContainer pipeline;
    pipeline.push_back(stage);
    pipeline.push_back(downstreamStage);
    ds->optimizeAt(pipeline.begin(), &pipeline);

    ASSERT_FALSE(ds->getMetadata().has_value());
    ASSERT_TRUE(sourcePtr->isMetadataStreamSkipped());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       SerializedElidedStageRoundTripsWithoutMetadata) {
    // Build the stage with metadata, then elide it via
    // optimizeAt because no downstream stage references $$SEARCH_META. The serialized result
    // is exactly the BSON a router ships to its shards.
    auto sourceStage = DocumentSource::parse(getExpCtx(), BSON("$collStats" << BSONObj())).front();
    auto stage = DocumentSourceInternalDocumentResultsAndMetadata::create(
        getExpCtx(), std::move(sourceStage), MetadataBindSpec("SEARCH_META"));
    auto* ds = dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stage.get());
    ASSERT(ds);
    ASSERT(ds->getMetadata().has_value());

    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    DocumentSourceContainer pipeline;
    pipeline.push_back(stage);
    pipeline.push_back(downstreamStage);
    ds->optimizeAt(pipeline.begin(), &pipeline);
    ASSERT_FALSE(ds->getMetadata().has_value());

    auto serialized = ds->serialize().getDocument().toBson();
    ASSERT_FALSE(serialized.getObjectField(kStageName).hasField("metadata"));

    // Shard side - rebuilt stage must come back with metadata absent,
    // proving the elision survives serialize -> parse.
    auto* roundTripped = parse(serialized);
    ASSERT_FALSE(roundTripped->getMetadata().has_value());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       OptimizeRetainsMetadataWhenDownstreamStageReferencesSearchMeta) {
    auto sourceStage = DocumentSourceMockSkipMetadataStream::create(getExpCtx());
    auto* sourcePtr = sourceStage.get();
    auto stage = DocumentSourceInternalDocumentResultsAndMetadata::create(
        getExpCtx(), std::move(sourceStage), MetadataBindSpec("SEARCH_META"));
    auto* ds = dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stage.get());
    ASSERT(ds);

    auto middleStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("meta" << "$$SEARCH_META")))
            .front();
    DocumentSourceContainer pipeline;
    pipeline.push_back(stage);
    pipeline.push_back(middleStage);
    pipeline.push_back(downstreamStage);
    ds->optimizeAt(pipeline.begin(), &pipeline);

    ASSERT_TRUE(ds->getMetadata().has_value());
    ASSERT_FALSE(sourcePtr->isMetadataStreamSkipped());
}

// Translation tests — verify REGISTER_AGG_STAGES_MAPPING expansion via buildPipeline.
TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       TranslateNoMetadataYieldsTwoExecStages) {
    // Exchange (1 consumer) + $replaceRoot only when there is no metadata.
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, /*metadata=*/boost::none, /*returnCursor=*/false);
    auto sourcePipeline = Pipeline::create({ds}, expCtx);
    auto execPipeline = exec::agg::buildPipeline(sourcePipeline->freeze());

    const auto& stages = execPipeline->getStages();
    ASSERT_EQ(stages.size(), 2u);
    ASSERT_EQ(std::string_view(stages[0]->getCommonStats().stageTypeStr),
              DocumentSourceExchange::kStageName);
    ASSERT_EQ(std::string_view(stages[1]->getCommonStats().stageTypeStr), "$replaceRoot"sv);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       TranslateWithMetadataYieldsThreeExecStages) {
    // Standalone path: Exchange (2 consumers) + $replaceRoot + $setVar.
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto meta = MetadataBindSpec::parse(BSON("as" << "SEARCH_META"));
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, meta, /*returnCursor=*/false);
    auto sourcePipeline = Pipeline::create({ds}, expCtx);
    auto execPipeline = exec::agg::buildPipeline(sourcePipeline->freeze());

    const auto& stages = execPipeline->getStages();
    ASSERT_EQ(stages.size(), 3u);
    ASSERT_EQ(std::string_view(stages[0]->getCommonStats().stageTypeStr),
              DocumentSourceExchange::kStageName);
    ASSERT_EQ(std::string_view(stages[1]->getCommonStats().stageTypeStr), "$replaceRoot"sv);
    ASSERT_EQ(std::string_view(stages[2]->getCommonStats().stageTypeStr),
              "$setVariableFromSubPipeline"sv);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       TranslateReturnCursorStashesMetaPipeline) {
    // Sharded path: Exchange (2 consumers) + $replaceRoot and meta pipeline stashed on DS.
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto meta = MetadataBindSpec::parse(BSON("as" << "SEARCH_META"));
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, meta, /*returnCursor=*/true);
    boost::intrusive_ptr<DocumentSourceInternalDocumentResultsAndMetadata> dsRef = ds;
    auto sourcePipeline = Pipeline::create({std::move(ds)}, expCtx);
    auto execPipeline = exec::agg::buildPipeline(sourcePipeline->freeze());

    const auto& stages = execPipeline->getStages();
    ASSERT_EQ(stages.size(), 2u);
    ASSERT_EQ(std::string_view(stages[0]->getCommonStats().stageTypeStr),
              DocumentSourceExchange::kStageName);
    ASSERT_EQ(std::string_view(stages[1]->getCommonStats().stageTypeStr), "$replaceRoot"sv);

    auto additionalCursorPipeline = dsRef->takeAdditionalCursorPipeline();
    ASSERT_NE(additionalCursorPipeline, nullptr);
    ASSERT_EQ(additionalCursorPipeline->pipelineType, CursorTypeEnum::SearchMetaResult);

    // Verify the stashed meta pipeline has exactly 3 stages: Exchange consumer + replaceRoot +
    // the $limit:1 that bounds the metadata stream.
    const auto& metaStages = additionalCursorPipeline->getSources();
    ASSERT_EQ(metaStages.size(), 3u);
    auto stageIt = metaStages.begin();
    ASSERT_EQ((*stageIt)->getSourceName(), DocumentSourceExchange::kStageName);
    ++stageIt;
    ASSERT_EQ(std::string_view((*stageIt)->getSourceName()), "$replaceRoot"sv);
    ++stageIt;
    ASSERT_EQ(std::string_view((*stageIt)->getSourceName()), "$limit"sv);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       TranslateNoMetadataReturnCursorIsNoop) {
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, /*metadata=*/boost::none, /*returnCursor=*/true);
    boost::intrusive_ptr<DocumentSourceInternalDocumentResultsAndMetadata> dsRef = ds;
    auto sourcePipeline = Pipeline::create({std::move(ds)}, expCtx);
    auto execPipeline = exec::agg::buildPipeline(sourcePipeline->freeze());

    const auto& stages = execPipeline->getStages();
    ASSERT_EQ(stages.size(), 2u);
    ASSERT_FALSE(dsRef->hasAdditionalCursorPipeline());
}

// Builds a shared ShardedPlanSpec with the given sort pattern and optional merge pipeline,
// ready for passing to setShardedPlan().
auto makeSharedPlanSpec(BSONObj sortPattern, std::vector<BSONObj> mergePipeline = {}) {
    ShardedPlanSpec ps;
    ps.setResultsSortPattern(std::move(sortPattern));
    if (!mergePipeline.empty()) {
        ps.setMetaMergePipeline(std::move(mergePipeline));
    }
    return std::make_shared<ShardedPlanSpec>(std::move(ps));
}

// DPL tests
TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       DistributedPlanLogicWithoutCallbackReturnsNone) {
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto meta = MetadataBindSpec::parse(BSON("as" << "SEARCH_META"));
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, meta, /*returnCursor=*/false);

    ASSERT_FALSE(ds->distributedPlanLogic(nullptr).has_value());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, CanMovePastBlocksSearchMetaReference) {
    auto expCtx = getExpCtx();
    auto projectWithMeta =
        DocumentSource::parse(expCtx, BSON("$project" << BSON("m" << "$$SEARCH_META"))).front();
    ASSERT_FALSE(
        DocumentSourceInternalDocumentResultsAndMetadata::canMovePastDuringSplit(*projectWithMeta));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       CanMovePastAllowsOrderPreservingStageWithoutSearchMeta) {
    auto expCtx = getExpCtx();
    // DocumentSourceCanSwapWithSort has preservesOrderAndMetadata = true and does not
    // reference $$SEARCH_META.
    auto orderPreservingStage = DocumentSourceCanSwapWithSort::create(expCtx);
    ASSERT_TRUE(DocumentSourceInternalDocumentResultsAndMetadata::canMovePastDuringSplit(
        *orderPreservingStage));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       CanMovePastBlocksNonOrderPreservingStage) {
    auto expCtx = getExpCtx();
    auto groupStage = DocumentSource::parse(expCtx, BSON("$group" << BSON("_id" << "$x"))).front();
    // $group does not preserve order and metadata.
    ASSERT_FALSE(
        DocumentSourceInternalDocumentResultsAndMetadata::canMovePastDuringSplit(*groupStage));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       DistributedPlanLogicWithCallbackNoMetadataSetsSortOnly) {
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, /*metadata=*/boost::none, /*returnCursor=*/false);

    auto spec = makeSharedPlanSpec(BSON("score" << 1));
    ds->setShardedPlan([spec](ExpressionContext*) -> const auto& { return *spec; });

    auto dpl = ds->distributedPlanLogic(nullptr);
    ASSERT_TRUE(dpl.has_value());
    ASSERT_TRUE(dpl->mergeSortPattern.has_value());
    ASSERT_BSONOBJ_EQ(*dpl->mergeSortPattern, BSON("score" << 1));
    ASSERT_TRUE(dpl->mergingStages.empty());
}

DEATH_TEST_F(DocumentSourceInternalDocumentResultsAndMetadataDeathTest,
             DistributedPlanLogicWithCallbackTassertsOnEmptySortPattern,
             "12625501") {
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto meta = MetadataBindSpec::parse(BSON("as" << "SEARCH_META"));
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, meta, /*returnCursor=*/false);

    // Empty sort pattern, resultsSortPattern left unset.
    auto spec =
        std::make_shared<DocumentSourceInternalDocumentResultsAndMetadata::ShardedPlanSpec>();
    ds->setShardedPlan([spec](ExpressionContext*) -> const auto& { return *spec; });

    ds->distributedPlanLogic(nullptr);
}

DEATH_TEST_F(DocumentSourceInternalDocumentResultsAndMetadataDeathTest,
             DistributedPlanLogicWithCallbackTassertsOnMissingMergePipeline,
             "12625502") {
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto meta = MetadataBindSpec::parse(BSON("as" << "SEARCH_META"));
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, meta, /*returnCursor=*/false);

    // metaMergePipeline intentionally absent — should tassert when metadata is configured.
    auto spec = makeSharedPlanSpec(BSON("score" << 1));
    ds->setShardedPlan([spec](ExpressionContext*) -> const auto& { return *spec; });

    ds->distributedPlanLogic(nullptr);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       DistributedPlanLogicWithCallbackSetsMergingStages) {
    auto expCtx = getExpCtx();
    auto queueStage = DocumentSourceQueue::create(expCtx, {});
    auto meta = MetadataBindSpec::parse(BSON("as" << "SEARCH_META"));
    auto ds = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, queueStage, meta, /*returnCursor=*/false);

    auto spec =
        makeSharedPlanSpec(BSON("score" << 1),
                           {BSON("$group" << BSON("_id" << BSONNULL << "meta"
                                                        << BSON("$mergeObjects" << "$payload")))});
    ds->setShardedPlan([spec](ExpressionContext*) -> const auto& { return *spec; });

    auto dpl = ds->distributedPlanLogic(nullptr);
    ASSERT_TRUE(dpl.has_value());
    ASSERT_TRUE(dpl->mergeSortPattern.has_value());
    ASSERT_BSONOBJ_EQ(*dpl->mergeSortPattern, BSON("score" << 1));
    ASSERT_EQ(dpl->mergingStages.size(), 1u);
    ASSERT_EQ(std::string_view(dpl->mergingStages.front()->getSourceName()),
              "$setVariableFromSubPipeline"sv);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       ShardedPlanSurvivesBsonRoundTripAndDrivesDistributedPlanLogic) {
    // Router side: stage carries a live provider supplied by the extension AST node.
    auto* original = parse(BSON(kStageName << kSourceWithMeta));
    auto spec =
        makeSharedPlanSpec(BSON("score" << -1),
                           {BSON("$group" << BSON("_id" << BSONNULL << "meta"
                                                        << BSON("$mergeObjects" << "$payload")))});
    original->setShardedPlan([spec](ExpressionContext*) -> const auto& { return *spec; });

    // Serialize as if shipping into a subpipeline on a shard; the callback can't cross BSON.
    query_shape::SerializationOptions opts;
    opts.isSerializingForRemoteDispatch = true;
    const auto roundTripped = original->serialize(opts).getDocument().toBson();

    // Shard side: re-parse the serialized BSON. No provider is attached; only the cached spec the
    // router stamped survives.
    auto* reparsed = parse(roundTripped);

    auto dpl = reparsed->distributedPlanLogic(nullptr);
    ASSERT_TRUE(dpl.has_value());
    ASSERT_TRUE(dpl->mergeSortPattern.has_value());
    ASSERT_BSONOBJ_EQ(*dpl->mergeSortPattern, BSON("score" << -1));
    ASSERT_EQ(dpl->mergingStages.size(), 1u);
    ASSERT_EQ(std::string_view(dpl->mergingStages.front()->getSourceName()),
              "$setVariableFromSubPipeline"sv);
}

}  // namespace
}  // namespace mongo
