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

#include "mongo/db/pipeline/document_source_single_document_transformation.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/query/explain_options.h"

#include <iterator>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

ALLOCATE_DOCUMENT_SOURCE_ID(singleDocumentTransformation,
                            DocumentSourceSingleDocumentTransformation::id)

DocumentSourceSingleDocumentTransformation::DocumentSourceSingleDocumentTransformation(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    std::unique_ptr<TransformerInterface> parsedTransform,
    const StringData name,
    bool isIndependentOfAnyCollection)
    : DocumentSource(name, pExpCtx),
      _name(std::string{name}),
      _isIndependentOfAnyCollection(isIndependentOfAnyCollection) {
    if (parsedTransform) {
        _transformationProcessor =
            std::make_shared<SingleDocumentTransformationProcessor>(std::move(parsedTransform));
    }
}

const char* DocumentSourceSingleDocumentTransformation::getSourceName() const {
    return _name.c_str();
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
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSkippingOrLimitingStage = true;
    constraints.isAllowedWithinUpdatePipeline = true;
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
    const SerializationOptions& opts) const {
    return Value(
        Document{{getSourceName(),
                  _transformationProcessor
                      ? _transformationProcessor->getTransformer().serializeTransformation(opts)
                      : _cachedStageOptions}});
}

projection_executor::ExclusionNode& DocumentSourceSingleDocumentTransformation::getExclusionNode() {
    invariant(getTransformerType() == TransformerInterface::TransformerType::kExclusionProjection);
    auto ret = dynamic_cast<projection_executor::ExclusionProjectionExecutor&>(
                   getTransformationProcessor()->getTransformer())
                   .getRoot();
    invariant(ret);
    return *ret;
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

DocumentSourceContainer::iterator DocumentSourceSingleDocumentTransformation::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    } else if (dynamic_cast<DocumentSourceSkip*>(std::next(itr)->get())) {
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
