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

#include "mongo/db/update/update_object_node.h"

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/update/field_checker.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/update_array_node.h"
#include "mongo/db/update/update_leaf_node.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

/**
 * Parses a field of the form $[<identifier>] into <identifier>. 'field' must be of the form
 * $[<identifier>]. Returns a non-ok status if 'field' is in the first position in the path or the
 * array filter identifier does not have a corresponding filter in 'arrayFilters'. Adds the
 * identifier to 'foundIdentifiers'.
 */
StatusWith<std::string> parseArrayFilterIdentifier(
    StringData field,
    size_t position,
    const FieldRef& fieldRef,
    const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
    std::set<std::string>& foundIdentifiers) {
    dassert(fieldchecker::isArrayFilterIdentifier(field));

    if (position == 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Cannot have array filter identifier (i.e. '$[<id>]') "
                                       "element in the first position in path '"
                                    << fieldRef.dottedField() << "'");
    }

    auto identifier = field.substr(2, field.size() - 3);

    if (!identifier.empty() && arrayFilters.find(identifier) == arrayFilters.end()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "No array filter found for identifier '" << identifier
                                    << "' in path '" << fieldRef.dottedField() << "'");
    }

    if (!identifier.empty()) {
        foundIdentifiers.emplace(identifier.toString());
    }

    return identifier.toString();
}

/**
 * Gets the child of 'element' named 'field', if it exists. Otherwise returns a non-ok element.
 */
mutablebson::Element getChild(mutablebson::Element element, StringData field) {
    if (element.getType() == BSONType::Object) {
        return element[field];
    } else if (element.getType() == BSONType::Array) {
        auto indexFromField = str::parseUnsignedBase10Integer(field);
        if (indexFromField) {
            return element.findNthChild(*indexFromField);
        }
    }
    return element.getDocument().end();
}

/**
 * Applies 'child' to the child of 'applyParams->element' named 'field' (which will create it, if it
 * does not exist). If 'applyParams->pathToCreate' is created, then 'applyParams->pathToCreate' is
 * moved to the end of 'applyParams->pathTaken', and 'applyParams->element' is advanced to the end
 * of 'applyParams->pathTaken'. Updates 'applyResult' based on whether 'child' was a noop or
 * affected indexes.
 */
void applyChild(const UpdateNode& child,
                StringData field,
                UpdateExecutor::ApplyParams* applyParams,
                UpdateNode::UpdateNodeApplyParams* updateNodeApplyParams,
                UpdateExecutor::ApplyResult* applyResult) {

    auto pathTakenSizeBefore = updateNodeApplyParams->pathTaken->fieldRef().numParts();

    // A non-ok value for childElement will indicate that we need to append 'field' to the
    // 'pathToCreate' FieldRef.
    auto childElement = applyParams->element.getDocument().end();
    invariant(!childElement.ok());
    if (!updateNodeApplyParams->pathToCreate->empty()) {
        // We're already traversing a path with elements that don't exist yet, so we will definitely
        // need to append.
    } else {
        childElement = getChild(applyParams->element, field);
    }

    if (childElement.ok()) {
        // The path we've traversed so far already exists in our document, and 'childElement'
        // represents the Element indicated by the 'field' name or index, which we indicate by
        // updating the 'pathTaken' FieldRef.
        updateNodeApplyParams->pathTaken->append(
            field,
            applyParams->element.getType() == BSONType::Array
                ? RuntimeUpdatePath::ComponentType::kArrayIndex
                : RuntimeUpdatePath::ComponentType::kFieldName);
    } else {
        // We are traversing path components that do not exist in our document. Any update modifier
        // that creates new path components (i.e., any modifiers that return true for
        // allowCreation()) will need to create this component, so we append it to the
        // 'pathToCreate' FieldRef. If the component cannot be created, pathsupport::createPathAt()
        // will provide a sensible PathNotViable UserError.
        childElement = applyParams->element;
        updateNodeApplyParams->pathToCreate->appendPart(field);
    }

    auto childApplyParams = *applyParams;
    childApplyParams.element = childElement;
    UpdateNode::UpdateNodeApplyParams childUpdateNodeApplyParams = *updateNodeApplyParams;
    auto childApplyResult = child.apply(childApplyParams, childUpdateNodeApplyParams);

    applyResult->noop = applyResult->noop && childApplyResult.noop;
    applyResult->containsDotsAndDollarsField =
        applyResult->containsDotsAndDollarsField || childApplyResult.containsDotsAndDollarsField;

    // Pop 'field' off of 'pathToCreate' or 'pathTaken'.
    if (!updateNodeApplyParams->pathToCreate->empty()) {
        updateNodeApplyParams->pathToCreate->removeLastPart();
    } else {
        updateNodeApplyParams->pathTaken->popBack();
    }

    // If the child is an internal node, it may have created 'pathToCreate' and moved 'pathToCreate'
    // to the end of 'pathTaken'. We should advance 'element' to the end of 'pathTaken'.
    if (updateNodeApplyParams->pathTaken->size() > pathTakenSizeBefore) {
        for (auto i = pathTakenSizeBefore; i < updateNodeApplyParams->pathTaken->size(); ++i) {
            applyParams->element = getChild(
                applyParams->element, updateNodeApplyParams->pathTaken->fieldRef().getPart(i));
            invariant(applyParams->element.ok());
        }
    } else if (!updateNodeApplyParams->pathToCreate->empty()) {

        // If the child is a leaf node, it may have created 'pathToCreate' without moving
        // 'pathToCreate' to the end of 'pathTaken'. We should move 'pathToCreate' to the end of
        // 'pathTaken' and advance 'element' to the end of 'pathTaken'.
        childElement =
            getChild(applyParams->element, updateNodeApplyParams->pathToCreate->getPart(0));
        if (childElement.ok()) {
            applyParams->element = childElement;
            updateNodeApplyParams->pathTaken->append(
                updateNodeApplyParams->pathToCreate->getPart(0),
                applyParams->element.getType() == BSONType::Array
                    ? RuntimeUpdatePath::ComponentType::kArrayIndex
                    : RuntimeUpdatePath::ComponentType::kFieldName);

            // Either the path was fully created or not created at all.
            for (size_t i = 1; i < updateNodeApplyParams->pathToCreate->numParts(); ++i) {
                const BSONType parentType = applyParams->element.getType();
                applyParams->element =
                    getChild(applyParams->element, updateNodeApplyParams->pathToCreate->getPart(i));
                invariant(applyParams->element.ok());
                updateNodeApplyParams->pathTaken->append(
                    updateNodeApplyParams->pathToCreate->getPart(i),
                    parentType == BSONType::Array ? RuntimeUpdatePath::ComponentType::kArrayIndex
                                                  : RuntimeUpdatePath::ComponentType::kFieldName);
            }

            updateNodeApplyParams->pathToCreate->clear();
        }
    }
}

BSONObj makeBSONForOperator(const std::vector<std::pair<std::string, BSONObj>>& updatesForOp) {
    BSONObjBuilder bob;
    for (const auto& [path, value] : updatesForOp)
        bob << path << value.firstElement();
    return bob.obj();
}

}  // namespace

// static
StatusWith<bool> UpdateObjectNode::parseAndMerge(
    UpdateObjectNode* root,
    modifiertable::ModifierType type,
    BSONElement modExpr,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
    std::set<std::string>& foundIdentifiers) {
    FieldRef fieldRef;
    if (type != modifiertable::ModifierType::MOD_RENAME) {
        // General case: Create a path in the tree according to the path specified in the field name
        // of the "modExpr" element.
        fieldRef.parse(modExpr.fieldNameStringData());
    } else {
        // Special case: For $rename modifiers, we add two nodes to the tree:
        // 1) a ConflictPlaceholderNode at the path specified in the field name of the "modExpr"
        //    element and
        auto status = parseAndMerge(root,
                                    modifiertable::ModifierType::MOD_CONFLICT_PLACEHOLDER,
                                    modExpr,
                                    expCtx,
                                    arrayFilters,
                                    foundIdentifiers);
        if (!status.isOK()) {
            return status;
        }

        // 2) a RenameNode at the path specified by the value of the "modExpr" element, which must
        //    be a string value.
        if (BSONType::String != modExpr.type()) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "The 'to' field for $rename must be a string: " << modExpr);
        }

        fieldRef.parse(modExpr.valueStringData());
    }

    // Check that the path is updatable.
    auto status = fieldchecker::isUpdatable(fieldRef);
    if (!status.isOK()) {
        return status;
    }

    // Check that there is at most one positional ($) part of the path and it is not in the first
    // position.
    size_t positionalIndex;
    size_t positionalCount;
    bool positional = fieldchecker::isPositional(fieldRef, &positionalIndex, &positionalCount);

    if (positional && positionalCount > 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Too many positional (i.e. '$') elements found in path '"
                                    << fieldRef.dottedField() << "'");
    }

    if (positional && positionalIndex == 0) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "Cannot have positional (i.e. '$') element in the first position in path '"
                << fieldRef.dottedField() << "'");
    }

    // Construct and initialize the leaf node.
    auto leaf = modifiertable::makeUpdateLeafNode(type);
    invariant(leaf);
    status = leaf->init(modExpr, expCtx);
    if (!status.isOK()) {
        return status;
    }

    // Create UpdateInternalNodes along the path.
    UpdateInternalNode* current = static_cast<UpdateInternalNode*>(root);
    for (FieldIndex i = 0; i < fieldRef.numParts() - 1; ++i) {
        auto fieldIsArrayFilterIdentifier =
            fieldchecker::isArrayFilterIdentifier(fieldRef.getPart(i));

        std::string childName;
        if (fieldIsArrayFilterIdentifier) {
            auto status = parseArrayFilterIdentifier(
                fieldRef.getPart(i), i, fieldRef, arrayFilters, foundIdentifiers);
            if (!status.isOK()) {
                return status.getStatus();
            }
            childName = status.getValue();
        } else {
            childName = fieldRef.getPart(i).toString();
        }

        auto child = current->getChild(childName);
        auto childShouldBeArrayNode =
            fieldchecker::isArrayFilterIdentifier(fieldRef.getPart(i + 1));
        if (child) {
            if ((childShouldBeArrayNode && child->type != UpdateNode::Type::Array) ||
                (!childShouldBeArrayNode && child->type != UpdateNode::Type::Object)) {
                return Status(ErrorCodes::ConflictingUpdateOperators,
                              str::stream() << "Updating the path '" << fieldRef.dottedField()
                                            << "' would create a conflict at '"
                                            << fieldRef.dottedSubstring(0, i + 1) << "'");
            }
        } else {
            std::unique_ptr<UpdateInternalNode> ownedChild;
            if (childShouldBeArrayNode) {
                ownedChild = std::make_unique<UpdateArrayNode>(arrayFilters);
            } else {
                ownedChild = std::make_unique<UpdateObjectNode>();
            }
            child = ownedChild.get();
            current->setChild(std::move(childName), std::move(ownedChild));
        }
        current = static_cast<UpdateInternalNode*>(child);
    }

    // Add the leaf node to the end of the path.
    auto fieldIsArrayFilterIdentifier =
        fieldchecker::isArrayFilterIdentifier(fieldRef.getPart(fieldRef.numParts() - 1));

    std::string childName;
    if (fieldIsArrayFilterIdentifier) {
        auto status = parseArrayFilterIdentifier(fieldRef.getPart(fieldRef.numParts() - 1),
                                                 fieldRef.numParts() - 1,
                                                 fieldRef,
                                                 arrayFilters,
                                                 foundIdentifiers);
        if (!status.isOK()) {
            return status.getStatus();
        }
        childName = status.getValue();
    } else {
        childName = fieldRef.getPart(fieldRef.numParts() - 1).toString();
    }

    if (current->getChild(childName)) {
        return Status(ErrorCodes::ConflictingUpdateOperators,
                      str::stream()
                          << "Updating the path '" << fieldRef.dottedField()
                          << "' would create a conflict at '" << fieldRef.dottedField() << "'");
    }
    current->setChild(std::move(childName), std::move(leaf));

    return positional;
}

// static
std::unique_ptr<UpdateNode> UpdateObjectNode::createUpdateNodeByMerging(
    const UpdateObjectNode& leftNode, const UpdateObjectNode& rightNode, FieldRef* pathTaken) {
    auto mergedNode = std::make_unique<UpdateObjectNode>();

    mergedNode->_children =
        createUpdateNodeMapByMerging(leftNode._children, rightNode._children, pathTaken);

    // The "positional" field ("$" notation) lives outside of the _children map, so we merge it
    // separately.
    mergedNode->_positionalChild = copyOrMergeAsNecessary(
        leftNode._positionalChild.get(), rightNode._positionalChild.get(), pathTaken, "$");

    // In Clang-3.9, we can just return mergedNode directly, but in 3.7, we need a std::move
    return std::move(mergedNode);
}

UpdateNode* UpdateObjectNode::getChild(const std::string& field) const {
    if (fieldchecker::isPositionalElement(field)) {
        return _positionalChild.get();
    }

    auto child = _children.find(field);
    if (child == _children.end()) {
        return nullptr;
    }
    return child->second.get();
}

void UpdateObjectNode::setChild(std::string field, std::unique_ptr<UpdateNode> child) {
    if (fieldchecker::isPositionalElement(field)) {
        invariant(!_positionalChild);
        _positionalChild = std::move(child);
    } else {
        invariant(_children.find(field) == _children.end());
        _children[std::move(field)] = std::move(child);
    }
}

BSONObj UpdateObjectNode::serialize() const {
    std::map<std::string, std::vector<std::pair<std::string, BSONObj>>> operatorOrientedUpdates;

    BSONObjBuilder bob;

    for (const auto& [pathPrefix, child] : _children) {
        auto path = FieldRef(pathPrefix);
        child->produceSerializationMap(&path, &operatorOrientedUpdates);
    }

    for (const auto& [op, updates] : operatorOrientedUpdates)
        bob << op << makeBSONForOperator(updates);

    return bob.obj();
}

UpdateExecutor::ApplyResult UpdateObjectNode::apply(
    ApplyParams applyParams, UpdateNodeApplyParams updateNodeApplyParams) const {
    bool applyPositional = _positionalChild.get();
    if (applyPositional) {
        uassert(ErrorCodes::BadValue,
                "The positional operator did not find the match needed from the query.",
                !applyParams.matchedField.empty());
    }

    auto applyResult = ApplyResult::noopResult();

    for (const auto& pair : _children) {
        // If this child has the same field name as the positional child, they must be merged and
        // applied.
        if (applyPositional && pair.first == applyParams.matchedField) {

            // Check if we have stored the result of merging the positional child with this child.
            auto mergedChild = _mergedChildrenCache.find(pair.first);
            if (mergedChild == _mergedChildrenCache.end()) {
                // The full path to the merged field is required for error reporting. In order to
                // modify the 'pathTaken' FieldRef, we need a (mutable) copy of it.
                FieldRef pathTakenFieldRefCopy(updateNodeApplyParams.pathTaken->fieldRef());
                for (size_t i = 0; i < updateNodeApplyParams.pathToCreate->numParts(); ++i) {
                    pathTakenFieldRefCopy.appendPart(
                        updateNodeApplyParams.pathToCreate->getPart(i));
                }
                pathTakenFieldRefCopy.appendPart(applyParams.matchedField);
                auto insertResult = _mergedChildrenCache.emplace(
                    std::make_pair(pair.first,
                                   UpdateNode::createUpdateNodeByMerging(
                                       *_positionalChild, *pair.second, &pathTakenFieldRefCopy)));
                for (FieldIndex i = 0; i < updateNodeApplyParams.pathToCreate->numParts() + 1;
                     ++i) {
                    pathTakenFieldRefCopy.removeLastPart();
                }
                invariant(insertResult.second);
                mergedChild = insertResult.first;
            }

            applyChild(*mergedChild->second.get(),
                       pair.first,
                       &applyParams,
                       &updateNodeApplyParams,
                       &applyResult);

            applyPositional = false;
            continue;
        }

        // If 'matchedField' is alphabetically before the current child, we should apply the
        // positional child now.
        if (applyPositional && applyParams.matchedField < pair.first) {
            applyChild(*_positionalChild.get(),
                       applyParams.matchedField,
                       &applyParams,
                       &updateNodeApplyParams,
                       &applyResult);
            applyPositional = false;
        }

        // Apply the current child.
        applyChild(*pair.second, pair.first, &applyParams, &updateNodeApplyParams, &applyResult);
    }

    // 'matchedField' is alphabetically after all children, so we apply it now.
    if (applyPositional) {
        applyChild(*_positionalChild.get(),
                   applyParams.matchedField,
                   &applyParams,
                   &updateNodeApplyParams,
                   &applyResult);
    }

    return applyResult;
}

}  // namespace mongo
