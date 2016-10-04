/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

using std::string;
using std::unique_ptr;

//
// InclusionNode
//

InclusionNode::InclusionNode(std::string pathToNode) : _pathToNode(std::move(pathToNode)) {}

void InclusionNode::optimize() {
    for (auto&& expressionIt : _expressions) {
        _expressions[expressionIt.first] = expressionIt.second->optimize();
    }
    for (auto&& childPair : _children) {
        childPair.second->optimize();
    }
}

void InclusionNode::injectExpressionContext(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    for (auto&& expressionIt : _expressions) {
        expressionIt.second->injectExpressionContext(expCtx);
    }

    for (auto&& childPair : _children) {
        childPair.second->injectExpressionContext(expCtx);
    }
}

void InclusionNode::serialize(MutableDocument* output, bool explain) const {
    // Always put "_id" first if it was included (implicitly or explicitly).
    if (_inclusions.find("_id") != _inclusions.end()) {
        output->addField("_id", Value(true));
    }

    for (auto&& includedField : _inclusions) {
        if (includedField == "_id") {
            // Handled above.
            continue;
        }
        output->addField(includedField, Value(true));
    }

    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            MutableDocument subDoc;
            childIt->second->serialize(&subDoc, explain);
            output->addField(field, subDoc.freezeToValue());
        } else {
            auto expressionIt = _expressions.find(field);
            invariant(expressionIt != _expressions.end());
            output->addField(field, expressionIt->second->serialize(explain));
        }
    }
}

void InclusionNode::addDependencies(DepsTracker* deps) const {
    for (auto&& includedField : _inclusions) {
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
        childPair.second->addDependencies(deps);
    }
}

void InclusionNode::applyInclusions(Document inputDoc, MutableDocument* outputDoc) const {
    auto it = inputDoc.fieldIterator();
    while (it.more()) {
        auto fieldPair = it.next();
        auto fieldName = fieldPair.first.toString();
        if (_inclusions.find(fieldName) != _inclusions.end()) {
            outputDoc->addField(fieldName, fieldPair.second);
            continue;
        }

        auto childIt = _children.find(fieldName);
        if (childIt != _children.end()) {
            outputDoc->addField(fieldName,
                                childIt->second->applyInclusionsToValue(fieldPair.second));
        }
    }
}

Value InclusionNode::applyInclusionsToValue(Value inputValue) const {
    if (inputValue.getType() == BSONType::Object) {
        MutableDocument output;
        applyInclusions(inputValue.getDocument(), &output);
        return output.freezeToValue();
    } else if (inputValue.getType() == BSONType::Array) {
        std::vector<Value> values = inputValue.getArray();
        for (auto it = values.begin(); it != values.end(); ++it) {
            *it = applyInclusionsToValue(*it);
        }
        return Value(std::move(values));
    } else {
        // This represents the case where we are including children of a field which does not have
        // any children. e.g. applying the projection {"a.b": true} to the document {a: 2}. It is
        // somewhat weird, but our semantics are to return a document without the field "a". To do
        // so, we return the "missing" value here.
        return Value();
    }
}

void InclusionNode::addComputedFields(MutableDocument* outputDoc, Variables* vars) const {
    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            outputDoc->setField(field,
                                childIt->second->addComputedFields(outputDoc->peek()[field], vars));
        } else {
            auto expressionIt = _expressions.find(field);
            invariant(expressionIt != _expressions.end());
            outputDoc->setField(field, expressionIt->second->evaluate(vars));
        }
    }
}

Value InclusionNode::addComputedFields(Value inputValue, Variables* vars) const {
    if (inputValue.getType() == BSONType::Object) {
        MutableDocument outputDoc(inputValue.getDocument());
        addComputedFields(&outputDoc, vars);
        return outputDoc.freezeToValue();
    } else if (inputValue.getType() == BSONType::Array) {
        std::vector<Value> values = inputValue.getArray();
        for (auto it = values.begin(); it != values.end(); ++it) {
            *it = addComputedFields(*it, vars);
        }
        return Value(std::move(values));
    } else {
        if (subtreeContainsComputedFields()) {
            // Our semantics in this case are to replace whatever existing value we find with a new
            // document of all the computed values. This case represents applying a projection like
            // {"a.b": {$literal: 1}} to the document {a: 1}. This should yield {a: {b: 1}}.
            MutableDocument outputDoc;
            addComputedFields(&outputDoc, vars);
            return outputDoc.freezeToValue();
        }
        // We didn't have any expressions, so just return the missing value.
        return Value();
    }
}

bool InclusionNode::subtreeContainsComputedFields() const {
    return (!_expressions.empty()) ||
        std::any_of(
               _children.begin(),
               _children.end(),
               [](const std::pair<const std::string, std::unique_ptr<InclusionNode>>& childPair) {
                   return childPair.second->subtreeContainsComputedFields();
               });
}

void InclusionNode::addComputedField(const FieldPath& path, boost::intrusive_ptr<Expression> expr) {
    if (path.getPathLength() == 1) {
        auto fieldName = path.fullPath();
        _expressions[fieldName] = expr;
        _orderToProcessAdditionsAndChildren.push_back(fieldName);
        return;
    }
    addOrGetChild(path.getFieldName(0))->addComputedField(path.tail(), expr);
}

void InclusionNode::addIncludedField(const FieldPath& path) {
    if (path.getPathLength() == 1) {
        _inclusions.insert(path.fullPath());
        return;
    }
    addOrGetChild(path.getFieldName(0))->addIncludedField(path.tail());
}

InclusionNode* InclusionNode::addOrGetChild(std::string field) {
    auto child = getChild(field);
    return child ? child : addChild(field);
}

InclusionNode* InclusionNode::getChild(string field) const {
    auto childIt = _children.find(field);
    return childIt == _children.end() ? nullptr : childIt->second.get();
}

InclusionNode* InclusionNode::addChild(string field) {
    invariant(!str::contains(field, "."));
    _orderToProcessAdditionsAndChildren.push_back(field);
    auto childPath = FieldPath::getFullyQualifiedPath(_pathToNode, field);
    auto insertedPair = _children.emplace(
        std::make_pair(std::move(field), stdx::make_unique<InclusionNode>(std::move(childPath))));
    return insertedPair.first->second.get();
}

//
// ParsedInclusionProjection
//

void ParsedInclusionProjection::parse(const BSONObj& spec,
                                      const VariablesParseState& variablesParseState) {
    // It is illegal to specify a projection with no output fields.
    bool atLeastOneFieldInOutput = false;

    // Tracks whether or not we should implicitly include "_id".
    bool idSpecified = false;

    for (auto elem : spec) {
        auto fieldName = elem.fieldNameStringData();
        idSpecified = idSpecified || fieldName == "_id" || fieldName.startsWith("_id.");
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
                _root->addIncludedField(FieldPath(elem.fieldName()));
                break;
            }
            case BSONType::Object: {
                // This is either an expression, or a nested specification.
                if (parseObjectAsExpression(fieldName, elem.Obj(), variablesParseState)) {
                    // It was an expression.
                    break;
                }

                // The field name might be a dotted path. If so, we need to keep adding children
                // to our tree until we create a child that represents that path.
                auto remainingPath = FieldPath(elem.fieldName());
                auto child = _root.get();
                while (remainingPath.getPathLength() > 1) {
                    child = child->addOrGetChild(remainingPath.getFieldName(0));
                    remainingPath = remainingPath.tail();
                }
                // It is illegal to construct an empty FieldPath, so the above loop ends one
                // iteration too soon. Add the last path here.
                child = child->addOrGetChild(remainingPath.fullPath());

                parseSubObject(elem.Obj(), variablesParseState, child);
                break;
            }
            default: {
                // This is a literal value.
                _root->addComputedField(FieldPath(elem.fieldName()),
                                        Expression::parseOperand(elem, variablesParseState));
            }
        }
    }

    if (!idSpecified) {
        // "_id" wasn't specified, so include it by default.
        atLeastOneFieldInOutput = true;
        _root->addIncludedField(FieldPath("_id"));
    }

    uassert(16403,
            str::stream() << "$project requires at least one output field: " << spec.toString(),
            atLeastOneFieldInOutput);
}

Document ParsedInclusionProjection::applyProjection(Document inputDoc, Variables* vars) const {
    // All expressions will be evaluated in the context of the input document, before any
    // transformations have been applied.
    vars->setRoot(inputDoc);

    MutableDocument output;
    _root->applyInclusions(inputDoc, &output);
    _root->addComputedFields(&output, vars);

    // Always pass through the metadata.
    output.copyMetaDataFrom(inputDoc);
    return output.freeze();
}

bool ParsedInclusionProjection::parseObjectAsExpression(
    StringData pathToObject,
    const BSONObj& objSpec,
    const VariablesParseState& variablesParseState) {
    if (objSpec.firstElementFieldName()[0] == '$') {
        // This is an expression like {$add: [...]}. We have already verified that it has only one
        // field.
        invariant(objSpec.nFields() == 1);
        _root->addComputedField(pathToObject.toString(),
                                Expression::parseExpression(objSpec, variablesParseState));
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
                node->addIncludedField(FieldPath(elem.fieldName()));
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
                auto child = node->addOrGetChild(fieldName);
                parseSubObject(elem.Obj(), variablesParseState, child);
                break;
            }
            default: {
                // This is a literal value.
                node->addComputedField(FieldPath(elem.fieldName()),
                                       Expression::parseOperand(elem, variablesParseState));
            }
        }
    }
}
}  // namespace parsed_aggregation_projection
}  // namespace mongo
