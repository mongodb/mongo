/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/update_object_node.h"

#include "mongo/db/update/field_checker.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/update_leaf_node.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

bool isPositionalElement(const std::string& field) {
    return field.length() == 1 && field[0] == '$';
}

}  // namespace

// static
StatusWith<bool> UpdateObjectNode::parseAndMerge(UpdateObjectNode* root,
                                                 modifiertable::ModifierType type,
                                                 BSONElement modExpr,
                                                 const CollatorInterface* collator) {
    // Check that the path is updatable.
    FieldRef fieldRef(modExpr.fieldNameStringData());
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
                                    << fieldRef.dottedField()
                                    << "'");
    }

    if (positional && positionalIndex == 0) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "Cannot have positional (i.e. '$') element in the first position in path '"
                << fieldRef.dottedField()
                << "'");
    }

    // Construct the leaf node.
    // TODO SERVER-28777: This should never fail because all modifiers are implemented.
    auto leaf = modifiertable::makeUpdateLeafNode(type);
    if (!leaf) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Cannot construct modifier of type " << type);
    }

    // Initialize the leaf node.
    status = leaf->init(modExpr, collator);
    if (!status.isOK()) {
        return status;
    }

    // Create UpdateObjectNodes along the path.
    UpdateObjectNode* current = root;
    for (size_t i = 0; i < fieldRef.numParts() - 1; ++i) {
        auto field = fieldRef.getPart(i).toString();
        auto child = current->getChild(field);
        if (child) {
            if (child->type != UpdateNode::Type::Object) {
                return Status(ErrorCodes::ConflictingUpdateOperators,
                              str::stream() << "Updating the path '" << fieldRef.dottedField()
                                            << "' would create a conflict at '"
                                            << fieldRef.dottedSubstring(0, i + 1)
                                            << "'");
            }
        } else {
            auto ownedChild = stdx::make_unique<UpdateObjectNode>();
            child = ownedChild.get();
            current->setChild(std::move(field), std::move(ownedChild));
        }
        current = static_cast<UpdateObjectNode*>(child);
    }

    // Add the leaf node to the end of the path.
    auto field = fieldRef.getPart(fieldRef.numParts() - 1).toString();
    if (current->getChild(field)) {
        return Status(ErrorCodes::ConflictingUpdateOperators,
                      str::stream() << "Updating the path '" << fieldRef.dottedField()
                                    << "' would create a conflict at '"
                                    << fieldRef.dottedField()
                                    << "'");
    }
    current->setChild(std::move(field), std::move(leaf));

    return positional;
}

// static
std::unique_ptr<UpdateNode> UpdateObjectNode::copyOrMergeAsNecessary(UpdateNode* leftNode,
                                                                     UpdateNode* rightNode,
                                                                     FieldRef* pathTaken,
                                                                     const std::string& nextField) {
    if (!leftNode && !rightNode) {
        return nullptr;
    } else if (!leftNode) {
        return rightNode->clone();
    } else if (!rightNode) {
        return leftNode->clone();
    } else {
        std::unique_ptr<FieldRef> updatedFieldRef(
            new FieldRef(pathTaken->dottedField() + "." + nextField));
        return UpdateNode::performMerge(*leftNode, *rightNode, updatedFieldRef.get());
    }
}

// static
std::unique_ptr<UpdateNode> UpdateObjectNode::performMerge(const UpdateObjectNode& leftNode,
                                                           const UpdateObjectNode& rightNode,
                                                           FieldRef* pathTaken) {
    auto mergedNode = stdx::make_unique<UpdateObjectNode>();

    // Get the union of the field names we know about among the leftNode and rightNode children.
    stdx::unordered_set<std::string> allFields;
    for (const auto& child : leftNode._children) {
        allFields.insert(child.first);
    }
    for (const auto& child : rightNode._children) {
        allFields.insert(child.first);
    }

    // Create an entry in mergedNode->children for all the fields we found.
    mergedNode->_children.reserve(allFields.size());
    for (const std::string& fieldName : allFields) {
        auto leftChildIt = leftNode._children.find(fieldName);
        auto rightChildIt = rightNode._children.find(fieldName);
        UpdateNode* leftChildPtr =
            (leftChildIt != leftNode._children.end()) ? leftChildIt->second.get() : nullptr;
        UpdateNode* rightChildPtr =
            (rightChildIt != rightNode._children.end()) ? rightChildIt->second.get() : nullptr;
        invariant(leftChildPtr || rightChildPtr);
        mergedNode->_children.insert(std::make_pair(
            fieldName, copyOrMergeAsNecessary(leftChildPtr, rightChildPtr, pathTaken, fieldName)));
    }

    // The "positional" field ("$" notation) lives outside of the _children map, so we merge it
    // separately.
    mergedNode->_positionalChild = copyOrMergeAsNecessary(
        leftNode._positionalChild.get(), rightNode._positionalChild.get(), pathTaken, "$");

    // In Clang-3.9, we can just return mergedNode directly, but in 3.7, we need a std::move
    return std::move(mergedNode);
}

UpdateNode* UpdateObjectNode::getChild(const std::string& field) const {
    if (isPositionalElement(field)) {
        return _positionalChild.get();
    }

    auto child = _children.find(field);
    if (child == _children.end()) {
        return nullptr;
    }
    return child->second.get();
}

void UpdateObjectNode::setChild(std::string field, std::unique_ptr<UpdateNode> child) {
    if (isPositionalElement(field)) {
        invariant(!_positionalChild);
        _positionalChild = std::move(child);
    } else {
        invariant(_children.find(field) == _children.end());
        _children[std::move(field)] = std::move(child);
    }
}

}  // namespace mongo
