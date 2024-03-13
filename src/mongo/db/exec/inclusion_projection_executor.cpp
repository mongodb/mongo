/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <absl/meta/type_traits.h>
#include <tuple>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::projection_executor {
using ComputedFieldsPolicy = ProjectionPolicies::ComputedFieldsPolicy;

Document FastPathEligibleInclusionNode::applyToDocument(const Document& inputDoc) const {
    if (auto outputDoc = tryApplyFastPathProjection(inputDoc)) {
        return outputDoc.get();
    }
    // A fast-path projection is not feasible, fall back to default implementation.
    return InclusionNode::applyToDocument(inputDoc);
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
    StringData oldName, StringData newName, const std::set<StringData>& reservedNames) {
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
    std::vector<std::tuple<StringData, boost::intrusive_ptr<Expression>, bool>>
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
        if (topLevelFieldNames.size() == 1 && topLevelFieldNames.count(oldName.toString()) == 1) {
            // Substitute newName for oldName in the expression.
            StringMap<std::string> renames;
            renames[oldName] = newName.toString();
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
            auto&& fieldName = std::get<0>(expressionSpec).toString();
            auto oldExpr = std::get<1>(expressionSpec);
            oldExpr->serialize().addToBsonObj(&bb, fieldName);

            if (std::get<2>(expressionSpec)) {
                // Replace the expression with an inclusion projected field.
                auto it = _projectedFields.insert(_projectedFields.end(), fieldName);
                _projectedFieldsSet.insert(StringData(*it));
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
    StringData oldName, StringData newName, const std::set<StringData>& reservedNames) {
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
    std::vector<std::pair<StringData, boost::intrusive_ptr<Expression>>> addFieldsExpressions;
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
        if (topLevelFieldNames.size() == 1 && topLevelFieldNames.count(oldName.toString()) == 1) {
            // Substitute newName for oldName in the expression.
            StringMap<std::string> renames;
            renames[oldName] = newName.toString();
            addFieldsExpressions.emplace_back(expressionIt->first,
                                              substituteInExpr(expressionIt->second, renames));
        } else {
            break;
        }
    }

    if (!addFieldsExpressions.empty()) {
        BSONObjBuilder bb;
        for (const auto& expressionSpec : addFieldsExpressions) {
            auto&& fieldName = expressionSpec.first.toString();
            auto expr = expressionSpec.second;
            expr->serialize().addToBsonObj(&bb, fieldName);

            // Remove the expression from this inclusion node.
            _expressions.erase(fieldName);
            _orderToProcessAdditionsAndChildren.erase(_orderToProcessAdditionsAndChildren.begin());
        }
        // If all expressions have been extracted, this inclusion node should be removed.
        return {bb.obj(), _orderToProcessAdditionsAndChildren.size() == 0};
    }
    return {BSONObj{}, false};
}

}  // namespace mongo::projection_executor
