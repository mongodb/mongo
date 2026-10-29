// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/wrapped_extension_source_hooks.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization final
    : public DocumentSourceMock,
      public WrappedExtensionSourceHooks {
public:
    static boost::intrusive_ptr<DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization>
    create(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        boost::intrusive_ptr<DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization>
            mock(new DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization(expCtx));
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

    void applyPipelineSuffixDependencies(const DepsTracker& deps,
                                         const std::set<std::string>& builtinVarRefs) override {
        _suffixDependenciesApplied = true;
        _appliedMetadataDeps = deps.metadataDeps();
        _appliedVariableRefs = builtinVarRefs;
    }

    bool wereSuffixDependenciesApplied() const {
        return _suffixDependenciesApplied;
    }

    const QueryMetadataBitSet& appliedMetadataDeps() const {
        return _appliedMetadataDeps;
    }

    const std::set<std::string>& appliedVariableRefs() const {
        return _appliedVariableRefs;
    }

    void dispatchInPlaceRules(
        rule_based_rewrites::pipeline::PipelineRewriteContext& ctx) const override {
        _inPlaceRulesDispatched = true;
    }

    bool wereInPlaceRulesDispatched() const {
        return _inPlaceRulesDispatched;
    }

private:
    DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization(
        const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    bool _metadataStreamSkipped = false;
    bool _suffixDependenciesApplied = false;
    QueryMetadataBitSet _appliedMetadataDeps;
    std::set<std::string> _appliedVariableRefs;
    mutable bool _inPlaceRulesDispatched = false;
};

// Fixture for tests exercising DocumentSourceInternalDocumentResultsAndMetadata's optimization-time
// interactions with its wrapped source: all of them wrap a fresh
// DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization in a
// DocumentSourceInternalDocumentResultsAndMetadata stage configured with SEARCH_META metadata.
class InternalDocumentResultsAndMetadataRulesTest : public AggregationContextFixture {
protected:
    boost::intrusive_ptr<DocumentSourceInternalDocumentResultsAndMetadata> makeWrappedStage() {
        auto sourceStage =
            DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization::create(
                getExpCtx());
        _sourcePtr = sourceStage.get();
        return DocumentSourceInternalDocumentResultsAndMetadata::create(
            getExpCtx(), std::move(sourceStage), MetadataBindSpec("SEARCH_META"));
    }

    DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization* sourcePtr() const {
        return _sourcePtr;
    }

    void runInPlaceRules_forTest(std::list<boost::intrusive_ptr<DocumentSource>> stages) {
        auto pipeline = Pipeline::create(std::move(stages), getExpCtx());
        rule_based_rewrites::pipeline::PipelineRewriteEngine engine({*pipeline}, 100);
        engine.applyRules(rule_based_rewrites::pipeline::PipelineRewriteContext::Tags::InPlace);
    }

private:
    DocumentSourceMockForInternalDocumentResultsAndMetadataOptimization* _sourcePtr = nullptr;
};

TEST_F(InternalDocumentResultsAndMetadataRulesTest, OptimizeElidesMetadataWhenNoSearchMetaRef) {
    auto stage = makeWrappedStage();
    ASSERT(stage->getMetadata().has_value());

    // Downstream $project does not reference $$SEARCH_META.
    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    DocumentSourceContainer pipeline;
    pipeline.push_back(stage);
    pipeline.push_back(downstreamStage);
    stage->optimizeAt(pipeline.begin(), &pipeline);

    ASSERT_FALSE(stage->getMetadata().has_value());
    ASSERT_TRUE(sourcePtr()->isMetadataStreamSkipped());
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest, OptimizeAppliesSuffixDependenciesToSource) {
    auto stage = makeWrappedStage();

    // Downstream $project references the searchSequenceToken metadata field and the $$NOW built-in
    // variable, so both the metadata dep and the variable ref must reach the wrapped source.
    auto downstreamStage =
        DocumentSource::parse(
            getExpCtx(),
            BSON("$project" << BSON("tok" << BSON("$meta" << "searchSequenceToken") << "ts"
                                          << "$$NOW")))
            .front();
    runInPlaceRules_forTest({stage, downstreamStage});

    ASSERT_TRUE(sourcePtr()->wereSuffixDependenciesApplied());
    ASSERT_TRUE(
        sourcePtr()->appliedMetadataDeps()[DocumentMetadataFields::MetaType::kSearchSequenceToken]);
    ASSERT_EQ(sourcePtr()->appliedVariableRefs().count("NOW"), 1u);
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest, OptimizeSkipsSuffixDepsWhenNeedsMerge) {
    // On shards, the merge pipeline's dependencies are invisible, so suffix dependencies must not
    // be forwarded to the wrapped source.
    getExpCtx()->setNeedsMerge(true);
    auto stage = makeWrappedStage();

    auto downstreamStage =
        DocumentSource::parse(
            getExpCtx(), BSON("$project" << BSON("tok" << BSON("$meta" << "searchSequenceToken"))))
            .front();
    runInPlaceRules_forTest({stage, downstreamStage});

    ASSERT_FALSE(sourcePtr()->wereSuffixDependenciesApplied());
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest, OptimizeSkipsSuffixDepsWhenNoDownstreamStages) {
    // When this is the last stage in the pipeline there is no suffix from which to derive
    // dependencies, so nothing should be forwarded to the wrapped source.
    auto stage = makeWrappedStage();

    runInPlaceRules_forTest({stage});

    ASSERT_FALSE(sourcePtr()->wereSuffixDependenciesApplied());
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest,
       OptimizeAppliesEmptySuffixDepsWhenNoDownstreamRef) {
    // A downstream stage that references neither metadata nor built-in variables still triggers
    // forwarding, but with an empty dependency set and no variable refs.
    auto stage = makeWrappedStage();

    auto downstreamStage =
        DocumentSource::parse(getExpCtx(), BSON("$project" << BSON("x" << 1))).front();
    runInPlaceRules_forTest({stage, downstreamStage});

    ASSERT_TRUE(sourcePtr()->wereSuffixDependenciesApplied());
    ASSERT_FALSE(sourcePtr()->appliedMetadataDeps().any());
    ASSERT_TRUE(sourcePtr()->appliedVariableRefs().empty());
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest, DispatchesInPlaceRulesToWrappedSource) {
    auto stage = makeWrappedStage();

    auto downstreamStage = DocumentSource::parse(getExpCtx(), BSON("$limit" << 5)).front();
    runInPlaceRules_forTest({stage, downstreamStage});

    ASSERT_TRUE(sourcePtr()->wereInPlaceRulesDispatched());
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest, DoesNotDispatchInPlaceRulesWhenNeedsMerge) {
    // On a split shard the pipeline suffix is incomplete, so the wrapped source's in-place rules
    // must not be dispatched against it.
    getExpCtx()->setNeedsMerge(true);
    auto stage = makeWrappedStage();

    auto downstreamStage = DocumentSource::parse(getExpCtx(), BSON("$limit" << 5)).front();
    runInPlaceRules_forTest({stage, downstreamStage});

    ASSERT_FALSE(sourcePtr()->wereInPlaceRulesDispatched());
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest,
       SkipsBothWrappedSourceRulesWhenExtensionsOptimizationsFlagOff) {
    // Neither rule may run any extensions-optimizations-API interaction while the flag is off.
    unittest::ServerParameterGuard guard("featureFlagExtensionsOptimizations", false);
    auto stage = makeWrappedStage();

    auto downstreamStage = DocumentSource::parse(getExpCtx(), BSON("$limit" << 5)).front();
    runInPlaceRules_forTest({stage, downstreamStage});

    ASSERT_FALSE(sourcePtr()->wereSuffixDependenciesApplied());
    ASSERT_FALSE(sourcePtr()->wereInPlaceRulesDispatched());
}

TEST_F(InternalDocumentResultsAndMetadataRulesTest, SkipsSourceWithoutHooksInterface) {
    // A source that doesn't implement WrappedExtensionSourceHooks must be handled gracefully.
    auto sourceStage = DocumentSource::parse(getExpCtx(), BSON("$collStats" << BSONObj())).front();
    auto stage = DocumentSourceInternalDocumentResultsAndMetadata::create(
        getExpCtx(), std::move(sourceStage), MetadataBindSpec("SEARCH_META"));

    auto downstreamStage = DocumentSource::parse(getExpCtx(), BSON("$limit" << 5)).front();
    runInPlaceRules_forTest({stage, downstreamStage});

    ASSERT_EQ(stage->getSourceStage()->getSourceName(), std::string_view("$collStats"));
}

}  // namespace
}  // namespace mongo
