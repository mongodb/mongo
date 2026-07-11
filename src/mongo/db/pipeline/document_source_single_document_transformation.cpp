// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_single_document_transformation.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/query/explain_options.h"

#include <iterator>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

ALLOCATE_DOCUMENT_SOURCE_ID(singleDocumentTransformation,
                            DocumentSourceSingleDocumentTransformation::id)

DocumentSourceSingleDocumentTransformation::DocumentSourceSingleDocumentTransformation(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    std::unique_ptr<TransformerInterface> parsedTransform,
    const std::string_view name,
    bool isIndependentOfAnyCollection)
    : DocumentSource(name, pExpCtx),
      _name(std::string{name}),
      _isIndependentOfAnyCollection(isIndependentOfAnyCollection) {
    if (parsedTransform) {
        _transformationProcessor =
            std::make_shared<SingleDocumentTransformationProcessor>(std::move(parsedTransform));
    }
}

std::string_view DocumentSourceSingleDocumentTransformation::getSourceName() const {
    return _name;
}

StageConstraints DocumentSourceSingleDocumentTransformation::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kAllowed,
                                 TransactionRequirement::kAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed,
                                 ChangeStreamRequirement::kAllowlist);
    constraints.preservesCardinality = true;
    // TODO SERVER-127594: audit preservesOrderAndMetadata for all TransformerTypes; $replaceRoot
    // and $replaceWith are fixed here to unblock vectorSearch storedSource:true sort optimization.
    if (_transformationProcessor &&
        _transformationProcessor->getTransformer().getType() ==
            TransformerInterface::TransformerType::kReplaceRoot) {
        constraints.preservesOrderAndMetadata = true;
    }
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSkippingOrLimitingStage = true;
    constraints.isAllowedWithinUpdatePipeline = true;
    constraints.outputDependsOnSingleInput = true;
    // This transformation could be part of a 'collectionless' change stream on an entire
    // database or cluster, mark as independent of any collection if so.
    constraints.isIndependentOfAnyCollection = _isIndependentOfAnyCollection;
    return constraints;
}

intrusive_ptr<DocumentSource> DocumentSourceSingleDocumentTransformation::optimize() {
    if (_transformationProcessor) {
        _transformationProcessor->getTransformer().optimize();

        // Note: This comes after the first call to optimize() to make sure the expression is
        // optimized so we can check if it's a no-op after things like constant folding.
        if (_transformationProcessor->getTransformer().isNoop()) {
            return nullptr;
        }
    }
    return this;
}

Value DocumentSourceSingleDocumentTransformation::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(
        Document{{getSourceName(),
                  _transformationProcessor
                      ? _transformationProcessor->getTransformer().serializeTransformation(opts)
                      : _cachedStageOptions}});
}

projection_executor::ExclusionNode& DocumentSourceSingleDocumentTransformation::getExclusionNode() {
    tassert(11282965,
            "Expecting exclusion projection transformation type",
            getTransformerType() == TransformerInterface::TransformerType::kExclusionProjection);
    auto ret = dynamic_cast<projection_executor::ExclusionProjectionExecutor&>(
                   getTransformationProcessor()->getTransformer())
                   .getRoot();
    tassert(11282964, "Missing ExclusionProjectionExecutor", ret);
    return *ret;
}

void DocumentSourceSingleDocumentTransformation::describeTransformation(
    document_transformation::DocumentOperationVisitor& visitor) const {
    _transformationProcessor->getTransformer().describeTransformation(visitor);
}

DocumentSourceContainer::iterator DocumentSourceSingleDocumentTransformation::maybeCoalesce(
    DocumentSourceContainer::iterator itr,
    DocumentSourceContainer* container,
    DocumentSourceSingleDocumentTransformation* nextSingleDocTransform) {
    // Adjacent exclusion projections can be coalesced by unioning their excluded fields.
    if (getTransformerType() == TransformerInterface::TransformerType::kExclusionProjection &&
        nextSingleDocTransform->getTransformerType() ==
            TransformerInterface::TransformerType::kExclusionProjection) {
        projection_executor::ExclusionNode& thisExclusionNode = getExclusionNode();
        projection_executor::ExclusionNode& nextExclusionNode =
            nextSingleDocTransform->getExclusionNode();

        auto isDotted = [](auto path) {
            return path.find('.') != std::string::npos;
        };
        OrderedPathSet thisExcludedPaths;
        thisExclusionNode.reportProjectedPaths(&thisExcludedPaths);
        if (std::any_of(thisExcludedPaths.begin(), thisExcludedPaths.end(), isDotted)) {
            return std::next(itr);
        }

        OrderedPathSet nextExcludedPaths;
        nextExclusionNode.reportProjectedPaths(&nextExcludedPaths);
        if (std::any_of(nextExcludedPaths.begin(), nextExcludedPaths.end(), isDotted)) {
            return std::next(itr);
        }
        for (const std::string& nextExcludedPathStr : nextExcludedPaths) {
            thisExclusionNode.addProjectionForPath(nextExcludedPathStr);
        }
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

DocumentSourceContainer::iterator DocumentSourceSingleDocumentTransformation::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(11282963, "Expecting DocumentSource iterator pointing to this stage", *itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    } else if (std::next(itr)->get()->isInstanceOf<DocumentSourceSkip>()) {
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    } else if (auto nextSingleDocTransform =
                   dynamic_cast<DocumentSourceSingleDocumentTransformation*>(
                       std::next(itr)->get())) {
        return maybeCoalesce(itr, container, nextSingleDocTransform);
    } else if (_transformationProcessor) {
        return _transformationProcessor->getTransformer().doOptimizeAt(itr, container);
    } else {
        return std::next(itr);
    }
}

DepsTracker::State DocumentSourceSingleDocumentTransformation::getDependencies(
    DepsTracker* deps) const {
    // Each parsed transformation is responsible for adding its own dependencies, and returning
    // the correct dependency return type for that transformation.
    return _transformationProcessor->getTransformer().addDependencies(deps);
}

void DocumentSourceSingleDocumentTransformation::addVariableRefs(
    std::set<Variables::Id>* refs) const {
    _transformationProcessor->getTransformer().addVariableRefs(refs);
}

DocumentSource::GetModPathsReturn DocumentSourceSingleDocumentTransformation::getModifiedPaths()
    const {
    return _transformationProcessor->getTransformer().getModifiedPaths();
}

}  // namespace mongo
