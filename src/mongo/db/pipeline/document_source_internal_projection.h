// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_projection_gen.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * The 'DocumentSourceInternalProjection' class (internal stage name '$_internalProjection') is used
 * to push aggregation pipeline $project and $addFields stages down to SBE. Both of these are
 * represented by a 'projection_ast::Projection' node since $addFields is implemented as a minor
 * variant of $project.
 */
class DocumentSourceInternalProjection final : public DocumentSource {
public:
    static constexpr std::string_view kStageNameInternal = "$_internalProjection"sv;

    DocumentSourceInternalProjection(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     DocumentSourceInternalProjectionSpec spec);

    DocumentSourceInternalProjection(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     BSONObj projectionSpec,
                                     InternalProjectionPolicyEnum policies)
        : DocumentSourceInternalProjection(
              pExpCtx, DocumentSourceInternalProjectionSpec(std::move(projectionSpec), policies)) {}

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {};

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);
        constraints.canSwapWithMatch = true;
        constraints.canSwapWithSkippingOrLimitingStage = true;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);


    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    const projection_ast::Projection& projection() const {
        return _projection;
    }

    const DocumentSourceInternalProjectionSpec& spec() const {
        return _stageSpec;
    }

private:
    // The specification for the $_internalProjection stage (defined by
    // document_source_internal_projection.idl).
    DocumentSourceInternalProjectionSpec _stageSpec;

    // The AST node representing this $project or $addFields.
    projection_ast::Projection _projection;
};
}  // namespace mongo
