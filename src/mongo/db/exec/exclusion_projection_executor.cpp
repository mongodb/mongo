// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/exclusion_projection_executor.h"

#include "mongo/db/query/compiler/dependency_analysis/document_transformation.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"

#include <algorithm>
#include <list>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::projection_executor {

Document FastPathEligibleExclusionNode::applyToDocument(const Document& inputDoc,
                                                        const EvaluationContext& ctx) const {
    if (auto outputDoc = tryApplyFastPathProjection(inputDoc)) {
        return outputDoc.get();
    }
    // A fast-path projection is not feasible, fall back to default implementation.
    return ExclusionNode::applyToDocument(inputDoc, ctx);
}

std::pair<BSONObj, bool> ExclusionNode::extractProjectOnFieldAndRename(std::string_view oldName,
                                                                       std::string_view newName) {
    BSONObjBuilder extractedExclusion;

    // Check for a projection directly on 'oldName'. For example, {oldName: 0}.
    if (auto it = _projectedFieldsSet.find(oldName); it != _projectedFieldsSet.end()) {
        extractedExclusion.append(newName, false);
        _projectedFieldsSet.erase(it);
        _projectedFields.remove(std::string(oldName));
    }

    // Check for a projection on subfields of 'oldName'. For example, {oldName: {a: 0, b: 0}}.
    if (auto it = _children.find(oldName); it != _children.end()) {
        extractedExclusion.append(newName, it->second->serialize({}).toBson());
        _children.erase(it);
    }

    if (auto it = std::find(_orderToProcessAdditionsAndChildren.begin(),
                            _orderToProcessAdditionsAndChildren.end(),
                            oldName);
        it != _orderToProcessAdditionsAndChildren.end()) {
        _orderToProcessAdditionsAndChildren.erase(it);
    }

    return {extractedExclusion.obj(), _projectedFields.empty() && _children.empty()};
}

ExclusionProjectionExecutor::ExclusionProjectionExecutor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    ProjectionPolicies policies,
    bool allowFastPath)
    : ProjectionExecutor(expCtx, policies),
      _root((allowFastPath && !internalQueryDisableExclusionProjectionFastPath)
                ? std::make_unique<FastPathEligibleExclusionNode>(policies)
                : std::make_unique<ExclusionNode>(policies)) {}

void ExclusionProjectionExecutor::describeTransformation(
    document_transformation::DocumentOperationVisitor& visitor) const {
    if (_rootReplacementExpression) {
        visitor(document_transformation::ReplaceRoot{});
        return;
    }
    _root->describeTransformation(visitor);
}

}  // namespace mongo::projection_executor
