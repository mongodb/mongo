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

/**
 * Applies 'child' to the child of 'element' named 'field' (which will create it, if it does not
 * exist). If 'pathToCreate' is created, then 'pathToCreate' is moved to the end of 'pathTaken', and
 * 'element' is advanced to the end of 'pathTaken'.
 */
void applyChild(const UpdateNode& child,
                StringData field,
                mutablebson::Element* element,
                FieldRef* pathToCreate,
                FieldRef* pathTaken,
                StringData matchedField,
                bool fromReplication,
                const UpdateIndexData* indexData,
                LogBuilder* logBuilder,
                bool* indexesAffected,
                bool* noop) {

    auto childElement = *element;
    auto pathTakenSizeBefore = pathTaken->numParts();

    // If 'field' exists in 'element', append 'field' to the end of 'pathTaken' and advance
    // 'childElement'. Otherwise, append 'field' to the end of 'pathToCreate'.
    if (pathToCreate->empty() && (childElement = (*element)[field]).ok()) {
        pathTaken->appendPart(field);
    } else {
        childElement = *element;
        pathToCreate->appendPart(field);
    }

    bool childAffectsIndexes = false;
    bool childNoop = false;

    uassertStatusOK(child.apply(childElement,
                                pathToCreate,
                                pathTaken,
                                matchedField,
                                fromReplication,
                                indexData,
                                logBuilder,
                                &childAffectsIndexes,
                                &childNoop));

    *indexesAffected = *indexesAffected || childAffectsIndexes;
    *noop = *noop && childNoop;

    // Pop 'field' off of 'pathToCreate' or 'pathTaken'.
    if (!pathToCreate->empty()) {
        pathToCreate->removeLastPart();
    } else {
        pathTaken->removeLastPart();
    }

    // If the child is an internal node, it may have created 'pathToCreate' and moved 'pathToCreate'
    // to the end of 'pathTaken'. We should advance 'element' to the end of 'pathTaken'.
    if (pathTaken->numParts() > pathTakenSizeBefore) {
        for (auto i = pathTakenSizeBefore; i < pathTaken->numParts(); ++i) {
            *element = (*element)[pathTaken->getPart(i)];
            invariant(element->ok());
        }
    } else if (!pathToCreate->empty()) {

        // If the child is a leaf node, it may have created 'pathToCreate' without moving
        // 'pathToCreate' to the end of 'pathTaken'. We should move 'pathToCreate' to the end of
        // 'pathTaken' and advance 'element' to the end of 'pathTaken'.
        childElement = (*element)[pathToCreate->getPart(0)];
        if (childElement.ok()) {
            *element = childElement;
            pathTaken->appendPart(pathToCreate->getPart(0));

            // Either the path was fully created or not created at all.
            for (size_t i = 1; i < pathToCreate->numParts(); ++i) {
                *element = (*element)[pathToCreate->getPart(i)];
                invariant(element->ok());
                pathTaken->appendPart(pathToCreate->getPart(i));
            }

            pathToCreate->clear();
        }
    }
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

namespace {
/**
 * Helper class for appending to a FieldRef for the duration of the current scope and then restoring
 * the FieldRef at the end of the scope.
 */
class FieldRefTempAppend {
public:
    FieldRefTempAppend(FieldRef& fieldRef, const std::string& part) : _fieldRef(fieldRef) {
        _fieldRef.appendPart(part);
    }

    ~FieldRefTempAppend() {
        _fieldRef.removeLastPart();
    }

private:
    FieldRef& _fieldRef;
};

/**
 * Helper for when performMerge wants to create a merged child from children that exist in two
 * merging nodes. If there is only one child (leftNode or rightNode is NULL), we clone it. If there
 * are two different children, we merge them recursively. If there are no children (leftNode and
 * rightNode are null), we return nullptr.
 */
std::unique_ptr<UpdateNode> copyOrMergeAsNecessary(UpdateNode* leftNode,
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
        FieldRefTempAppend tempAppend(*pathTaken, nextField);
        return UpdateNode::createUpdateNodeByMerging(*leftNode, *rightNode, pathTaken);
    }
}
}

// static
std::unique_ptr<UpdateNode> UpdateObjectNode::createUpdateNodeByMerging(
    const UpdateObjectNode& leftNode, const UpdateObjectNode& rightNode, FieldRef* pathTaken) {
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

Status UpdateObjectNode::apply(mutablebson::Element element,
                               FieldRef* pathToCreate,
                               FieldRef* pathTaken,
                               StringData matchedField,
                               bool fromReplication,
                               const UpdateIndexData* indexData,
                               LogBuilder* logBuilder,
                               bool* indexesAffected,
                               bool* noop) const {
    *indexesAffected = false;
    *noop = true;

    bool applyPositional = _positionalChild.get();
    if (applyPositional) {
        uassert(ErrorCodes::BadValue,
                "The positional operator did not find the match needed from the query.",
                !matchedField.empty());
    }

    // Capture arguments to applyChild() to avoid code duplication.
    auto applyChildClosure = [=, &element](const UpdateNode& child, StringData field) {
        applyChild(child,
                   field,
                   &element,
                   pathToCreate,
                   pathTaken,
                   matchedField,
                   fromReplication,
                   indexData,
                   logBuilder,
                   indexesAffected,
                   noop);
    };

    for (const auto& pair : _children) {

        // If this child has the same field name as the positional child, they must be merged and
        // applied.
        if (applyPositional && pair.first == matchedField) {

            // Check if we have stored the result of merging the positional child with this child.
            auto mergedChild = _mergedChildrenCache.find(pair.first);
            if (mergedChild == _mergedChildrenCache.end()) {

                // The full path to the merged field is required for error reporting.
                for (size_t i = 0; i < pathToCreate->numParts(); ++i) {
                    pathTaken->appendPart(pathToCreate->getPart(i));
                }
                pathTaken->appendPart(matchedField);
                auto insertResult = _mergedChildrenCache.emplace(
                    std::make_pair(pair.first,
                                   UpdateNode::createUpdateNodeByMerging(
                                       *_positionalChild, *pair.second, pathTaken)));
                for (size_t i = 0; i < pathToCreate->numParts() + 1; ++i) {
                    pathTaken->removeLastPart();
                }
                invariant(insertResult.second);
                mergedChild = insertResult.first;
            }

            applyChildClosure(*mergedChild->second.get(), pair.first);

            applyPositional = false;
            continue;
        }

        // If 'matchedField' is alphabetically before the current child, we should apply the
        // positional child now.
        if (applyPositional && matchedField < pair.first) {
            applyChildClosure(*_positionalChild.get(), matchedField);
            applyPositional = false;
        }

        // Apply the current child.
        applyChildClosure(*pair.second, pair.first);
    }

    // 'matchedField' is alphabetically after all children, so we apply it now.
    if (applyPositional) {
        applyChildClosure(*_positionalChild.get(), matchedField);
    }

    return Status::OK();
}

}  // namespace mongo
