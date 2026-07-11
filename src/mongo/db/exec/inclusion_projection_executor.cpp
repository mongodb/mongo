// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/inclusion_projection_executor.h"

#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <tuple>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::projection_executor {
using ComputedFieldsPolicy = ProjectionPolicies::ComputedFieldsPolicy;

Document FastPathEligibleInclusionNode::applyToDocument(const Document& inputDoc,
                                                        const EvaluationContext& ctx) const {
    if (auto outputDoc = tryApplyFastPathProjection(inputDoc)) {
        return outputDoc.get();
    }
    // A fast-path projection is not feasible, fall back to default implementation.
    return InclusionNode::applyToDocument(inputDoc, ctx);
}

namespace {
// A helper function to substitute field path element in expression using the 'renames' map.
boost::intrusive_ptr<Expression> substituteInExpr(boost::intrusive_ptr<Expression> ex,
                                                  StringMap<std::string> renames) {
    SubstituteFieldPathWalker substituteWalker(renames);
    auto substExpr = expression_walker::walk<Expression>(ex.get(), &substituteWalker);
    if (substExpr.get() != nullptr) {
        return substExpr.release();
    }
    return ex;
};

/**
 * Returns a vector of top-level dependencies where each index i in the vector corresponds to the
 * dependencies from the ith expression according to 'orderToProcess'. Will return boost::none if
 * any expression needs the whole document.
 */
boost::optional<std::vector<OrderedPathSet>> getTopLevelDeps(
    const std::vector<std::string>& orderToProcess,
    const StringMap<boost::intrusive_ptr<Expression>>& expressions,
    const StringMap<std::unique_ptr<ProjectionNode>>& children) {
    std::vector<OrderedPathSet> topLevelDeps;
    for (const auto& field : orderToProcess) {
        DepsTracker deps;
        if (auto exprIt = expressions.find(field); exprIt != expressions.end()) {
            expression::addDependencies(exprIt->second.get(), &deps);
        } else {
            // Each expression in orderToProcess should either be in expressions or children.
            auto childIt = children.find(field);
            tassert(6657000, "Unable to calculate dependencies", childIt != children.end());
            childIt->second->reportDependencies(&deps);
        }

        if (deps.needWholeDocument) {
            return boost::none;
        }

        topLevelDeps.push_back(
            DepsTracker::simplifyDependencies(deps.fields, DepsTracker::TruncateToRootLevel::yes));
    }
    return topLevelDeps;
}

/**
 * Returns whether or not there is an expression in the projection which depends on 'field' other
 * than the expression which computes 'field'. For example, given field "a" and projection
 * {a: "$b", c: {$sum: ["$a", 5]}}, return true. Given field "a" and projection
 * {a: {$sum: ["$a", 5]}, c: "$b"}, return false. 'field' should be a top level path.
 */
bool computedExprDependsOnField(const std::vector<OrderedPathSet>& topLevelDeps,
                                const std::string& field,
                                const size_t fieldIndex) {
    for (size_t i = 0; i < topLevelDeps.size(); i++) {
        if (i != fieldIndex && topLevelDeps[i].count(field) > 0) {
            return true;
        }
    }
    return false;
}
}  // namespace

std::pair<BSONObj, bool> InclusionNode::extractComputedProjectionsInProject(
    std::string_view oldName,
    std::string_view newName,
    const std::set<std::string_view>& reservedNames) {
    if (_policies.computedFieldsPolicy != ComputedFieldsPolicy::kAllowComputedFields) {
        return {BSONObj{}, false};
    }

    boost::optional<std::vector<OrderedPathSet>> topLevelDeps =
        getTopLevelDeps(_orderToProcessAdditionsAndChildren, _expressions, _children);

    // If one of the expression requires the whole document, then we should not extract the
    // projection and topLevelDeps will not hold any field names.
    if (!topLevelDeps) {
        return {BSONObj{}, false};
    }

    // Auxiliary vector with extracted computed projections: <name, expression, replacement
    // strategy>. If the replacement strategy flag is true, the expression is replaced with a
    // projected field. If it is false - the expression is replaced with an identity projection.
    std::vector<std::tuple<std::string_view, boost::intrusive_ptr<Expression>, bool>>
        addFieldsExpressions;
    bool replaceWithProjField = true;
    for (size_t i = 0; i < _orderToProcessAdditionsAndChildren.size(); i++) {
        auto&& field = _orderToProcessAdditionsAndChildren[i];

        if (reservedNames.count(field) > 0) {
            // Do not pushdown computed projection with reserved name.
            replaceWithProjField = false;
            continue;
        }

        auto expressionIt = _expressions.find(field);
        if (expressionIt == _expressions.end()) {
            // After seeing the first dotted path expression we need to replace computed
            // projections with identity projections to preserve the field order.
            replaceWithProjField = false;
            continue;
        }

        // Do not extract a computed projection if it is computing a value that other fields in the
        // same projection depend on. If the extracted $addFields were to be placed before this
        // projection, the dependency with the common name would be shadowed by the computed
        // projection.
        if (computedExprDependsOnField(topLevelDeps.get(), field, i)) {
            replaceWithProjField = false;
            continue;
        }

        const auto& topLevelFieldNames = topLevelDeps.get()[i];
        if (topLevelFieldNames.size() == 1 && topLevelFieldNames.count(std::string{oldName}) == 1) {
            // Substitute newName for oldName in the expression.
            StringMap<std::string> renames;
            renames[oldName] = std::string{newName};
            addFieldsExpressions.emplace_back(expressionIt->first,
                                              substituteInExpr(expressionIt->second, renames),
                                              replaceWithProjField);
        } else {
            // After seeing a computed expression that depends on other fields, we need to preserve
            // the order by replacing following computed projections with identity projections.
            replaceWithProjField = false;
        }
    }

    if (!addFieldsExpressions.empty()) {
        BSONObjBuilder bb;
        for (const auto& expressionSpec : addFieldsExpressions) {
            std::string fieldName{std::get<0>(expressionSpec)};
            auto oldExpr = std::get<1>(expressionSpec);

            // If the $addFields spec field name itself is using the old field name, then we need to
            // rename the field as well.
            auto addFieldsFieldName = oldName == fieldName ? newName : fieldName;
            oldExpr->serialize().addToBsonObj(&bb, addFieldsFieldName);

            if (std::get<2>(expressionSpec)) {
                // Replace the expression with an inclusion projected field.
                auto it = _projectedFields.insert(_projectedFields.end(), fieldName);
                _projectedFieldsSet.insert(std::string_view(*it));
                _expressions.erase(fieldName);
                // Only computed projections at the beginning of the list were marked to become
                // projected fields. The new projected field is at the beginning of the
                // _orderToProcessAdditionsAndChildren list.
                _orderToProcessAdditionsAndChildren.erase(
                    _orderToProcessAdditionsAndChildren.begin());
            } else {
                // Replace the expression with identity projection.
                auto newExpr = ExpressionFieldPath::createPathFromString(
                    oldExpr->getExpressionContext(),
                    fieldName,
                    oldExpr->getExpressionContext()->variablesParseState);
                _expressions[fieldName] = newExpr;
            }
        }
        return {bb.obj(), false};
    }
    return {BSONObj{}, false};
}

std::pair<BSONObj, bool> InclusionNode::extractComputedProjectionsInAddFields(
    std::string_view oldName,
    std::string_view newName,
    const std::set<std::string_view>& reservedNames) {
    if (_policies.computedFieldsPolicy != ComputedFieldsPolicy::kAllowComputedFields) {
        return {BSONObj{}, false};
    }

    boost::optional<std::vector<OrderedPathSet>> topLevelDeps =
        getTopLevelDeps(_orderToProcessAdditionsAndChildren, _expressions, _children);

    // If one of the expression requires the whole document, then we should not extract the
    // projection and topLevelDeps will not hold any field names.
    if (!topLevelDeps) {
        return {BSONObj{}, false};
    }

    // Auxiliary vector with extracted computed projections: <name, expression>.
    // To preserve the original fields order, only projections at the beginning of the
    // _orderToProcessAdditionsAndChildren list can be extracted for pushdown.
    std::vector<std::pair<std::string_view, boost::intrusive_ptr<Expression>>> addFieldsExpressions;
    for (size_t i = 0; i < _orderToProcessAdditionsAndChildren.size(); i++) {
        auto&& field = _orderToProcessAdditionsAndChildren[i];
        // Do not extract for pushdown computed projection with reserved name.
        if (reservedNames.count(field) > 0) {
            break;
        }

        auto expressionIt = _expressions.find(field);
        if (expressionIt == _expressions.end()) {
            break;
        }

        // Do not extract a computed projection if it is computing a value that other fields in the
        // same projection depend on. If the extracted $addFields were to be placed before this
        // projection, the dependency with the common name would be shadowed by the computed
        // projection.
        if (computedExprDependsOnField(topLevelDeps.get(), field, i)) {
            break;
        }

        auto& topLevelFieldNames = topLevelDeps.get()[i];
        if (topLevelFieldNames.size() == 1 && topLevelFieldNames.count(std::string{oldName}) == 1) {
            // Substitute newName for oldName in the expression.
            StringMap<std::string> renames;
            renames[oldName] = std::string{newName};
            addFieldsExpressions.emplace_back(expressionIt->first,
                                              substituteInExpr(expressionIt->second, renames));
        } else {
            break;
        }
    }

    if (!addFieldsExpressions.empty()) {
        BSONObjBuilder bb;
        for (const auto& expressionSpec : addFieldsExpressions) {
            auto&& fieldName = std::string{expressionSpec.first};
            auto expr = expressionSpec.second;

            // If the $addFields spec field name itself is using the old field name, then we need to
            // rename the field as well since it would have been the new meta field. For example,
            // {$addFields: {m: "$m.sub"}} where the 'm' is the meta field. If this $addFields is
            // pushed down before the unpack bucket, it should be {$addFields: {meta: "$meta.sub"}}
            // because this $addFields will hide the original meta and become the new meta.
            auto addFieldsFieldName = oldName == fieldName ? newName : fieldName;
            expr->serialize().addToBsonObj(&bb, addFieldsFieldName);

            // Remove the expression from this inclusion node.
            _expressions.erase(fieldName);
            _orderToProcessAdditionsAndChildren.erase(_orderToProcessAdditionsAndChildren.begin());
        }
        // If all expressions have been extracted, this inclusion node should be removed.
        return {bb.obj(), _orderToProcessAdditionsAndChildren.size() == 0};
    }
    return {BSONObj{}, false};
}

void InclusionProjectionExecutor::describeTransformation(
    document_transformation::DocumentOperationVisitor& visitor) const {
    visitor(document_transformation::ReplaceRoot{!_rootReplacementExpression /*isEmpty*/});
    if (_rootReplacementExpression) {
        return;
    }
    _root->describeTransformation(visitor);
}

}  // namespace mongo::projection_executor
