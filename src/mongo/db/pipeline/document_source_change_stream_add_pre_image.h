// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <set>
#include <string>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamAddPreImage);
using ChangeStreamAddPreImageLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamAddPreImageStageParams>;

/**
 * Part of the change stream API machinery used to look up the pre-image of a document.
 *
 * The identifier of pre-image is in "preImageId" field of the incoming document. The pre-image is
 * set to "fullDocumentBeforeChange" field of the returned document.
 */
class DocumentSourceChangeStreamAddPreImage final : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamAddPreImage"sv;
    static constexpr std::string_view kFullDocumentBeforeChangeFieldName =
        DocumentSourceChangeStream::kFullDocumentBeforeChangeField;
    static constexpr std::string_view kPreImageIdFieldName =
        DocumentSourceChangeStream::kPreImageIdField;

    /**
     * Creates a DocumentSourceChangeStreamAddPreImage stage.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamAddPreImage> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static boost::intrusive_ptr<DocumentSourceChangeStreamAddPreImage> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceChangeStreamAddPreImage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          FullDocumentBeforeChangeModeEnum mode)
        : DocumentSourceInternalChangeStreamStage(kStageName, expCtx),
          _fullDocumentBeforeChangeMode(mode) {
        tassert(11294809,
                "ChangeStreamAddPreImage stage should never be created with "
                "FullDocumentBeforeChangeMode::kOff.",
                _fullDocumentBeforeChangeMode != FullDocumentBeforeChangeModeEnum::kOff);
    }

    /**
     * Only modifies: "fullDocumentBeforeChange" and "preImageId".
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {
            GetModPathsReturn::Type::kFiniteSet,
            {std::string{kFullDocumentBeforeChangeFieldName}, std::string{kPreImageIdFieldName}},
            {}};
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        tassert(11294808,
                "Expecting pipeline to be either unsplit or split for merging",
                pipeState != PipelineSplitState::kSplitForShards);
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);
        constraints.preservesCardinality = true;
        constraints.canSwapWithMatch = true;
        constraints.consumesLogicalCollectionData = false;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        deps->fields.insert(std::string{DocumentSourceChangeStream::kOperationTypeField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kPreImageIdField});

        if (_fullDocumentBeforeChangeMode == FullDocumentBeforeChangeModeEnum::kRequired) {
            // These fields are only needed to generate error messages when a required pre-image
            // cannot be found.
            deps->fields.insert(std::string{DocumentSourceChangeStream::kClusterTimeField});
            deps->fields.insert(std::string{DocumentSourceChangeStream::kNamespaceField});
            deps->fields.insert(std::string{DocumentSourceChangeStream::kTxnNumberField});
        }

        // This stage does not restrict the output fields to a finite set, and has no impact on
        // whether metadata is available or needed.
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    Value doSerialize(const query_shape::SerializationOptions& opts =
                          query_shape::SerializationOptions{}) const final;

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamAddPreImageToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // Determines whether pre-images are strictly required or may be included only when available.
    FullDocumentBeforeChangeModeEnum _fullDocumentBeforeChangeMode =
        FullDocumentBeforeChangeModeEnum::kOff;
};

}  // namespace mongo
