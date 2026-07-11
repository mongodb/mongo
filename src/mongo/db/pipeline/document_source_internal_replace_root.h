// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Represents a $replaceRoot pipeline stage that can be translated to SBE instead of executing as a
 * DocumentSourceSingleDocumentTransformation.
 */
class DocumentSourceInternalReplaceRoot final : public DocumentSource {
public:
    static constexpr std::string_view kStageNameInternal = "$_internalReplaceRoot"sv;

    DocumentSourceInternalReplaceRoot(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                      boost::intrusive_ptr<Expression> newRoot)
        : DocumentSource(kStageNameInternal, pExpCtx), _newRoot(newRoot) {}

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

    boost::intrusive_ptr<Expression> newRootExpression() const {
        return _newRoot;
    }

private:
    // The parsed "newRoot" argument to the $replaceRoot stage.
    boost::intrusive_ptr<Expression> _newRoot;
};
}  // namespace mongo
