// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_projection.h"

#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"

#include <string_view>

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(_internalProjection, DocumentSourceInternalProjection::id)

namespace {
ProjectionPolicies lookUpPolicies(InternalProjectionPolicyEnum policiesId) {
    switch (policiesId) {
        case InternalProjectionPolicyEnum::kAggregate:
            return ProjectionPolicies::aggregateProjectionPolicies();
        case InternalProjectionPolicyEnum::kAddFields:
            return ProjectionPolicies::addFieldsProjectionPolicies();
    }
    MONGO_UNREACHABLE;
}
}  // namespace

/**
 * DocumentSourceInternalProjectionSpec constructor.
 */
DocumentSourceInternalProjection::DocumentSourceInternalProjection(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    DocumentSourceInternalProjectionSpec spec)
    : DocumentSource(kStageNameInternal, pExpCtx),
      _stageSpec(std::move(spec)),
      _projection(projection_ast::parseAndAnalyze(
          pExpCtx, _stageSpec.getSpec(), lookUpPolicies(_stageSpec.getPolicies()))) {}

std::string_view DocumentSourceInternalProjection::getSourceName() const {
    return kStageNameInternal;
}

DocumentSourceContainer::iterator DocumentSourceInternalProjection::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(11282992, "Expecting DocumentSource iterator pointing to this stage", *itr == this);
    return itr;
}

Value DocumentSourceInternalProjection::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _stageSpec.toBSON()}});
}
}  // namespace mongo
