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

#include "mongo/db/exec/projection_node.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::projection_executor {
using ArrayRecursionPolicy = ProjectionPolicies::ArrayRecursionPolicy;
using ComputedFieldsPolicy = ProjectionPolicies::ComputedFieldsPolicy;
using DefaultIdPolicy = ProjectionPolicies::DefaultIdPolicy;

ProjectionNode::ProjectionNode(ProjectionPolicies policies, std::string pathToNode)
    : _policies(policies), _pathToNode(std::move(pathToNode)) {}

void ProjectionNode::addProjectionForPath(const FieldPath& path) {
    // Enforce that this method can only be called on the root node.
    tassert(7241722, "can only add projection to path from the root node", _pathToNode.empty());
    _addProjectionForPath(path);
}

void ProjectionNode::_addProjectionForPath(const FieldPath& path) {
    makeOptimizationsStale();
    if (path.getPathLength() == 1) {
        // Add to projection unless we already have `path` as a projection.
        if (!_projectedFieldsSet.contains(path.fullPath())) {
            auto it = _projectedFields.insert(_projectedFields.end(), path.fullPath());
            _projectedFieldsSet.insert(StringData(*it));
        }
    } else {
        // FieldPath can't be empty, so it is safe to obtain the first path component here.
        addOrGetChild(std::string{path.getFieldName(0)})->_addProjectionForPath(path.tail());
    }
}

void ProjectionNode::addExpressionForPath(const FieldPath& path,
                                          boost::intrusive_ptr<Expression> expr) {
    // Enforce that this method can only be called on the root node.
    tassert(7241723, "can only add expression to path from the root node", _pathToNode.empty());
    _addExpressionForPath(path, std::move(expr));
}

void ProjectionNode::_addExpressionForPath(const FieldPath& path,
                                           boost::intrusive_ptr<Expression> expr) {
    makeOptimizationsStale();
    // If the computed fields policy is 'kBanComputedFields', we should never reach here.
    tassert(7241724,
            "computed fields must be allowed in inclusion projections",
            _policies.computedFieldsPolicy == ComputedFieldsPolicy::kAllowComputedFields);

    // We're going to add an expression either to this node, or to some child of this node.
    // In any case, the entire subtree will contain at least one computed field.
    _subtreeContainsComputedFields = true;

    if (path.getPathLength() == 1) {
        auto fieldName = path.fullPath();
        _expressions[fieldName] = expr;
        _orderToProcessAdditionsAndChildren.push_back(fieldName);
        return;
    }
    // FieldPath can't be empty, so it is safe to obtain the first path component here.
    addOrGetChild(std::string{path.getFieldName(0)})->_addExpressionForPath(path.tail(), expr);
}

boost::intrusive_ptr<Expression> ProjectionNode::getExpressionForPath(const FieldPath& path) const {
    // The FieldPath always conatins at least one field.
    auto fieldName = std::string{path.getFieldName(0)};

    if (path.getPathLength() == 1) {
        if (_expressions.find(fieldName) != _expressions.end()) {
            return _expressions.at(fieldName);
        }
        return nullptr;
    }
    if (auto child = getChild(fieldName)) {
        return child->getExpressionForPath(path.tail());
    }
    return nullptr;
}

ProjectionNode* ProjectionNode::addOrGetChild(const std::string& field) {
    makeOptimizationsStale();
    auto child = getChild(field);
    return child ? child : addChild(field);
}

ProjectionNode* ProjectionNode::addChild(const std::string& field) {
    makeOptimizationsStale();
    tassert(7241725,
            "field for child in projection cannot contain a path or '.'",
            !str::contains(field, "."));
    _orderToProcessAdditionsAndChildren.push_back(field);
    auto insertedPair = _children.emplace(std::make_pair(field, makeChild(field)));
    return insertedPair.first->second.get();
}

ProjectionNode* ProjectionNode::getChild(const std::string& field) const {
    auto childIt = _children.find(field);
    return childIt == _children.end() ? nullptr : childIt->second.get();
}

Document ProjectionNode::applyToDocument(const Document& inputDoc) const {
    // Defer to the derived class to initialize the output document, then apply.
    MutableDocument outputDoc{initializeOutputDocument(inputDoc)};
    applyProjections(inputDoc, &outputDoc);

    if (_subtreeContainsComputedFields) {
        applyExpressions(inputDoc, &outputDoc);
    }

    // Make sure that we always pass through any metadata present in the input doc.
    if (inputDoc.metadata()) {
        outputDoc.copyMetaDataFrom(inputDoc);
    }
    return outputDoc.freeze();
}

void ProjectionNode::applyProjections(const Document& inputDoc, MutableDocument* outputDoc) const {
    // Iterate over the input document so that the projected document retains its field ordering.
    auto it = inputDoc.fieldIterator();
    size_t projectedFields = 0;

    bool isIncl = isIncluded();

    while (it.more()) {
        auto fieldName = it.fieldName();

        if (_projectedFieldsSet.find(fieldName) != _projectedFieldsSet.end()) {
            if (isIncl) {
                outputProjectedField(fieldName, it.next().second, outputDoc);
            } else {
                outputProjectedField(fieldName, Value(), outputDoc);
                it.advance();
            }
            ++projectedFields;
        } else if (auto childIt = _children.find(fieldName); childIt != _children.end()) {
            outputProjectedField(
                fieldName, childIt->second->applyProjectionsToValue(it.next().second), outputDoc);
            ++projectedFields;
        } else {
            it.advance();
        }

        // Check if we can avoid reading from the document any further.
        if (_maxFieldsToProject && _maxFieldsToProject <= projectedFields) {
            break;
        }
    }
}

Value ProjectionNode::applyProjectionsToValue(Value inputValue) const {
    if (inputValue.getType() == BSONType::object) {
        MutableDocument outputSubDoc{initializeOutputDocument(inputValue.getDocument())};
        applyProjections(inputValue.getDocument(), &outputSubDoc);
        return outputSubDoc.freezeToValue();
    } else if (inputValue.getType() == BSONType::array) {
        std::vector<Value> values;
        values.reserve(inputValue.getArrayLength());
        for (const auto& input : inputValue.getArray()) {
            // If this is a nested array and our policy is to not recurse, skip the array.
            // Otherwise, descend into the array and project each element individually.
            const bool shouldSkip = input.isArray() &&
                _policies.arrayRecursionPolicy == ArrayRecursionPolicy::kDoNotRecurseNestedArrays;
            auto value = (shouldSkip ? transformSkippedValueForOutput(input)
                                     : applyProjectionsToValue(input));
            // If subtree contains computed fields, we need to keep missing values to apply
            // expressions in the next step. They will either become objects or will be cleaned up
            // after applying the expressions.
            if (!value.missing() || _subtreeContainsComputedFields) {
                values.push_back(std::move(value));
            }
        }
        return Value(std::move(values));
    } else {
        // This represents the case where we are projecting children of a field which does not have
        // any children; for instance, applying the projection {"a.b": true} to the document {a: 2}.
        return transformSkippedValueForOutput(inputValue);
    }
}

void ProjectionNode::outputProjectedField(StringData field, Value val, MutableDocument* doc) const {
    doc->setField(field, val);
}

void ProjectionNode::applyExpressions(const Document& root, MutableDocument* outputDoc) const {
    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            outputDoc->setField(field,
                                childIt->second->applyExpressionsToValue(
                                    root, outputDoc->peek()[StringData{field}]));
        } else {
            auto expressionIt = _expressions.find(field);
            tassert(7241726,
                    "reached end of expression iterator, but trying to evaluate the expression",
                    expressionIt != _expressions.end());
            outputDoc->setField(
                field,
                expressionIt->second->evaluate(
                    root, &expressionIt->second->getExpressionContext()->variables));
        }
    }
}

Value ProjectionNode::applyExpressionsToValue(const Document& root, Value inputValue) const {
    if (inputValue.getType() == BSONType::object) {
        MutableDocument outputDoc(inputValue.getDocument());
        applyExpressions(root, &outputDoc);
        return outputDoc.freezeToValue();
    } else if (inputValue.getType() == BSONType::array) {
        std::vector<Value> values;
        values.reserve(inputValue.getArrayLength());
        for (const auto& input : inputValue.getArray()) {
            auto value = applyExpressionsToValue(root, input);
            if (!value.missing()) {
                values.push_back(std::move(value));
            }
        }
        return Value(std::move(values));
    } else {
        if (_subtreeContainsComputedFields) {
            // Our semantics in this case are to replace whatever existing value we find with a new
            // document of all the computed values. This case represents applying a projection like
            // {"a.b": {$literal: 1}} to the document {a: 1}. This should yield {a: {b: 1}}.
            MutableDocument outputDoc;
            applyExpressions(root, &outputDoc);
            return outputDoc.freezeToValue();
        }
        // We didn't have any expressions, so just skip this value.
        return transformSkippedValueForOutput(inputValue);
    }
}

void ProjectionNode::reportProjectedPaths(OrderedPathSet* projectedPaths) const {
    for (auto&& projectedField : _projectedFields) {
        projectedPaths->insert(FieldPath::getFullyQualifiedPath(_pathToNode, projectedField));
    }

    for (auto&& childPair : _children) {
        childPair.second->reportProjectedPaths(projectedPaths);
    }
}

void ProjectionNode::reportComputedPaths(OrderedPathSet* computedPaths,
                                         StringMap<std::string>* renamedPaths,
                                         StringMap<std::string>* complexRenamedPaths) const {
    for (auto&& computedPair : _expressions) {
        // The expression's path is the concatenation of the path to this node, plus the field name
        // associated with the expression.
        auto exprPath = FieldPath::getFullyQualifiedPath(_pathToNode, computedPair.first);
        auto exprComputedPaths = computedPair.second->getComputedPaths(exprPath);
        computedPaths->insert(exprComputedPaths.paths.begin(), exprComputedPaths.paths.end());

        if (renamedPaths) {
            for (auto&& rename : exprComputedPaths.renames) {
                (*renamedPaths)[rename.first] = rename.second;
            }
        } else {
            // If caller is asking us not to report into 'renamedPaths', then report into
            // 'computedPaths' instead.
            for (auto&& rename : exprComputedPaths.renames) {
                computedPaths->insert(rename.first);
            }
        }

        if (complexRenamedPaths) {
            for (auto&& complexRename : exprComputedPaths.complexRenames) {
                (*complexRenamedPaths)[complexRename.first] = complexRename.second;
            }
        }
    }

    for (auto&& childPair : _children) {
        // Below the top level, do not track renames: everything is a computed path.
        // For example we can't consider {$addFields: {'a.b': "$x"}} to be a rename,
        // because if 'a' is an array then 'a.b' can point to zero or more than one
        // location in the document.
        //
        // If we knew which fields are not arrays, we could be more precise: in this
        // example if 'a' is not an array, then 'a.b' will write to exactly one place
        // in the document.
        childPair.second->reportComputedPaths(computedPaths, nullptr, nullptr);
    }
}

void ProjectionNode::optimize() {
    for (auto&& expressionIt : _expressions) {
        _expressions[expressionIt.first] = expressionIt.second->optimize();
    }
    for (auto&& childPair : _children) {
        childPair.second->optimize();
    }

    _maxFieldsToProject = maxFieldsToProject();
}

Document ProjectionNode::serialize(const SerializationOptions& options) const {
    MutableDocument outputDoc;
    serialize(&outputDoc, options);
    return outputDoc.freeze();
}

void ProjectionNode::serialize(MutableDocument* output, const SerializationOptions& options) const {
    // Determine the boolean value for projected fields in the explain output.
    const bool projVal = isIncluded();

    // Always put "_id" first if it was projected (implicitly or explicitly).
    if (_projectedFieldsSet.find("_id") != _projectedFieldsSet.end()) {
        output->addField(options.serializeFieldPath("_id"), Value(projVal));
    }

    for (auto&& projectedField : _projectedFields) {
        if (projectedField != "_id") {
            output->addField(options.serializeFieldPathFromString(projectedField), Value(projVal));
        }
    }

    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            MutableDocument subDoc;
            childIt->second->serialize(&subDoc, options);
            output->addField(options.serializeFieldPathFromString(field), subDoc.freezeToValue());
        } else {
            tassert(7241727,
                    "computed fields must be allowed in inclusion projections.",
                    _policies.computedFieldsPolicy == ComputedFieldsPolicy::kAllowComputedFields);
            auto expressionIt = _expressions.find(field);
            tassert(7241728,
                    "reached end of the expression iterator",
                    expressionIt != _expressions.end());

            auto isExpressionObject = dynamic_cast<ExpressionObject*>(expressionIt->second.get());

            if (isExpressionObject) {
                output->addField(
                    options.serializeFieldPathFromString(field),
                    Value(Document{{"$expr", expressionIt->second->serialize(options)}}));
            } else {
                output->addField(options.serializeFieldPathFromString(field),
                                 expressionIt->second->serialize(options));
            }
        }
    }
}
}  // namespace mongo::projection_executor
