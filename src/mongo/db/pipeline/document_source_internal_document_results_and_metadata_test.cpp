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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

constexpr StringData kStageName = "$_internalDocumentResultsAndMetadata"_sd;

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
        auto specObj = specElem.embeddedObject().getOwned();
        auto spec =
            DocumentSourceResultsAndMetadataSpec::parse(specObj, IDLParserContext(kStageName));
        InternalDocumentResultsAndMetadataStageParams params(std::move(spec));
        _stages = DocumentSourceInternalDocumentResultsAndMetadata::createFromStageParams(
            params, getExpCtx());
        ASSERT_EQ(_stages.size(), 1u);
        auto* docResultsAndMetadata =
            dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(_stages.front().get());
        ASSERT(docResultsAndMetadata);
        return docResultsAndMetadata;
    }

private:
    DocumentSourceContainer _stages;
};

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, ParsesFullSpec) {
    auto* stage = parse(BSON(kStageName << kFullSpec));
    ASSERT_EQ(stage->getSourceStage()->getSourceName(), StringData("$collStats"));
    ASSERT(stage->getMetadata().has_value());
    ASSERT_EQ(stage->getMetadata()->getAs(), "SEARCH_META");
    ASSERT_TRUE(stage->getReturnCursor());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, ParsesSpecWithoutMetadata) {
    auto* stage = parse(BSON(kStageName << kSourceOnly));
    ASSERT_FALSE(stage->getMetadata().has_value());
    ASSERT_FALSE(stage->getReturnCursor());
}

using DocumentSourceInternalDocumentResultsAndMetadataDeathTest =
    DocumentSourceInternalDocumentResultsAndMetadataTest;

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

    auto serialized = stage->serialize(SerializationOptions{});
    ASSERT_BSONOBJ_EQ(serialized.getDocument().toBson(), BSON(kStageName << kFullSpec));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, SerializesWithoutMetadata) {
    auto* stage = parse(BSON(kStageName << kSourceOnly));

    auto serialized = stage->serialize(SerializationOptions{});
    ASSERT_BSONOBJ_EQ(serialized.getDocument().toBson(), BSON(kStageName << kSourceOnly));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       SerializeIncludesReturnCursorWhenFalse) {
    auto* stage = parse(BSON(kStageName << kSourceWithMeta));

    auto serialized = stage->serialize(SerializationOptions{});
    auto bson = serialized.getDocument().toBson();
    auto inner = bson.getObjectField(kStageName);
    ASSERT_FALSE(inner["returnCursor"].Bool());
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest, SerializesWithIdentifierTransform) {
    auto* stage = parse(BSON(kStageName << kSourceWithMeta));

    auto serialized = stage->serialize(SerializationOptions::kMarkIdentifiers_FOR_TEST);
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

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       OptimizeElidesMetadataWhenNoSearchMetaRef) {
    auto sourceStage = DocumentSource::parse(getExpCtx(), BSON("$collStats" << BSONObj())).front();
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
       OptimizeSerializesWithoutMetadataAfterElision) {
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

    auto serialized = ds->serialize(SerializationOptions{});
    auto bson = serialized.getDocument().toBson();
    auto inner = bson.getObjectField(kStageName);
    ASSERT_FALSE(inner.hasField("metadata"));
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       OptimizeNoOpWhenMetadataAlreadyAbsent) {
    auto* ds = parse(BSON(kStageName << kSourceOnly));
    ASSERT_FALSE(ds->getMetadata().has_value());

    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    DocumentSourceContainer pipeline;
    pipeline.push_back(ds);
    pipeline.push_back(downstreamStage);
    ds->optimizeAt(pipeline.begin(), &pipeline);

    ASSERT_FALSE(ds->getMetadata().has_value());
    ASSERT_EQ(pipeline.size(), 2u);
}

TEST_F(DocumentSourceInternalDocumentResultsAndMetadataTest,
       OptimizeRetainsMetadataWhenDownstreamStageReferencesSearchMeta) {
    auto sourceStage = DocumentSource::parse(getExpCtx(), BSON("$collStats" << BSONObj())).front();
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
}

}  // namespace
}  // namespace mongo
