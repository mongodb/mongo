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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/parsed_inclusion_projection.h"

#include <algorithm>

namespace mongo {

namespace parsed_aggregation_projection {

//
// InclusionNode
//

InclusionNode::InclusionNode(ProjectionPolicies policies, std::string pathToNode)
    : ProjectionNode(policies, std::move(pathToNode)) {}

InclusionNode* InclusionNode::addOrGetChild(const std::string& field) {
    return static_cast<InclusionNode*>(ProjectionNode::addOrGetChild(field));
}

void InclusionNode::reportDependencies(DepsTracker* deps) const {
    for (auto&& includedField : _projectedFields) {
        deps->fields.insert(FieldPath::getFullyQualifiedPath(_pathToNode, includedField));
    }

    if (!_pathToNode.empty() && !_expressions.empty()) {
        // The shape of any computed fields in the output will change depending on if the field is
        // an array or not, so in addition to any dependencies of the expression itself, we need to
        // add this field to our dependencies.
        deps->fields.insert(_pathToNode);
    }

    for (auto&& expressionPair : _expressions) {
        expressionPair.second->addDependencies(deps);
    }
    for (auto&& childPair : _children) {
        childPair.second->reportDependencies(deps);
    }
}

//
// ParsedInclusionProjection
//

void ParsedInclusionProjection::parse(const BSONObj& spec) {
    // It is illegal to specify an inclusion with no output fields.
    bool atLeastOneFieldInOutput = false;

    // Tracks whether or not we should apply the default _id projection policy.
    bool idSpecified = false;

    for (auto elem : spec) {
        auto fieldName = elem.fieldNameStringData();
        idSpecified = idSpecified || fieldName == "_id"_sd || fieldName.startsWith("_id."_sd);
        if (fieldName == "_id") {
            const bool idIsExcluded = (!elem.trueValue() && (elem.isNumber() || elem.isBoolean()));
            if (idIsExcluded) {
                // Ignoring "_id" here will cause it to be excluded from result documents.
                _idExcluded = true;
                continue;
            }

            // At least part of "_id" is included or a computed field. Fall through to below to
            // parse what exactly "_id" was specified as.
        }

        atLeastOneFieldInOutput = true;
        switch (elem.type()) {
            case BSONType::Bool:
            case BSONType::NumberInt:
            case BSONType::NumberLong:
            case BSONType::NumberDouble:
            case BSONType::NumberDecimal: {
                // This is an inclusion specification.
                invariant(elem.trueValue());
                _root->addProjectionForPath(FieldPath(elem.fieldName()));
                break;
            }
            case BSONType::Object: {
                // This is either an expression, or a nested specification.
                if (parseObjectAsExpression(fieldName, elem.Obj(), _expCtx->variablesParseState)) {
                    // It was an expression.
                    break;
                }

                // The field name might be a dotted path. If so, we need to keep adding children
                // to our tree until we create a child that represents that path.
                auto remainingPath = FieldPath(elem.fieldName());
                auto* child = _root.get();
                while (remainingPath.getPathLength() > 1) {
                    child = child->addOrGetChild(remainingPath.getFieldName(0).toString());
                    remainingPath = remainingPath.tail();
                }
                // It is illegal to construct an empty FieldPath, so the above loop ends one
                // iteration too soon. Add the last path here.
                child = child->addOrGetChild(remainingPath.fullPath());

                parseSubObject(elem.Obj(), _expCtx->variablesParseState, child);
                break;
            }
            default: {
                // This is a literal value.
                _root->addExpressionForPath(
                    FieldPath(elem.fieldName()),
                    Expression::parseOperand(_expCtx, elem, _expCtx->variablesParseState));
            }
        }
    }

    if (!idSpecified) {
        // _id wasn't specified, so apply the default _id projection policy here.
        if (_policies.idPolicy == ProjectionPolicies::DefaultIdPolicy::kExcludeId) {
            _idExcluded = true;
        } else {
            atLeastOneFieldInOutput = true;
            _root->addProjectionForPath(FieldPath("_id"));
        }
    }

    uassert(16403,
            str::stream() << "$project requires at least one output field: " << spec.toString(),
            atLeastOneFieldInOutput);
}

Document ParsedInclusionProjection::applyProjection(const Document& inputDoc) const {
    // All expressions will be evaluated in the context of the input document, before any
    // transformations have been applied.
    return _root->applyToDocument(inputDoc);
}

bool ParsedInclusionProjection::parseObjectAsExpression(
    StringData pathToObject,
    const BSONObj& objSpec,
    const VariablesParseState& variablesParseState) {
    if (objSpec.firstElementFieldName()[0] == '$') {
        // This is an expression like {$add: [...]}. We have already verified that it has only one
        // field.
        invariant(objSpec.nFields() == 1);
        _root->addExpressionForPath(
            pathToObject, Expression::parseExpression(_expCtx, objSpec, variablesParseState));
        return true;
    }
    return false;
}

void ParsedInclusionProjection::parseSubObject(const BSONObj& subObj,
                                               const VariablesParseState& variablesParseState,
                                               InclusionNode* node) {
    for (auto elem : subObj) {
        invariant(elem.fieldName()[0] != '$');
        // Dotted paths in a sub-object have already been disallowed in
        // ParsedAggregationProjection's parsing.
        invariant(elem.fieldNameStringData().find('.') == std::string::npos);

        switch (elem.type()) {
            case BSONType::Bool:
            case BSONType::NumberInt:
            case BSONType::NumberLong:
            case BSONType::NumberDouble:
            case BSONType::NumberDecimal: {
                // This is an inclusion specification.
                invariant(elem.trueValue());
                node->addProjectionForPath(FieldPath(elem.fieldName()));
                break;
            }
            case BSONType::Object: {
                // This is either an expression, or a nested specification.
                auto fieldName = elem.fieldNameStringData().toString();
                if (parseObjectAsExpression(
                        FieldPath::getFullyQualifiedPath(node->getPath(), fieldName),
                        elem.Obj(),
                        variablesParseState)) {
                    break;
                }
                auto* child = node->addOrGetChild(fieldName);
                parseSubObject(elem.Obj(), variablesParseState, child);
                break;
            }
            default: {
                // This is a literal value.
                node->addExpressionForPath(
                    FieldPath(elem.fieldName()),
                    Expression::parseOperand(_expCtx, elem, variablesParseState));
            }
        }
    }
}

bool ParsedInclusionProjection::isEquivalentToDependencySet(const BSONObj& deps) const {
    std::set<std::string> preservedPaths;
    _root->reportProjectedPaths(&preservedPaths);
    size_t numDependencies = 0;
    for (auto&& dependency : deps) {
        if (!dependency.trueValue()) {
            // This is not an included field, so move on.
            continue;
        }

        if (preservedPaths.find(dependency.fieldNameStringData().toString()) ==
            preservedPaths.end()) {
            return false;
        }
        ++numDependencies;
    }

    if (numDependencies != preservedPaths.size()) {
        return false;
    }

    // If the inclusion has any computed fields or renamed fields, then it's not a subset.
    return !_root->subtreeContainsComputedFields();
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo
