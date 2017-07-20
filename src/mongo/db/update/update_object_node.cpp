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
#include "mongo/db/update/update_array_node.h"
#include "mongo/db/update/update_leaf_node.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/stringutils.h"

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
                                    << fieldRef.dottedField()
                                    << "'");
    }

    auto identifier = field.substr(2, field.size() - 3);

    if (!identifier.empty() && arrayFilters.find(identifier) == arrayFilters.end()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "No array filter found for identifier '" << identifier
                                    << "' in path '"
                                    << fieldRef.dottedField()
                                    << "'");
    }

    if (!identifier.empty()) {
        foundIdentifiers.emplace(identifier.toString());
    }

    return identifier.toString();
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
                bool validateForStorage,
                const FieldRefSet& immutablePaths,
                const UpdateIndexData* indexData,
                LogBuilder* logBuilder,
                bool* indexesAffected,
                bool* noop) {

    auto pathTakenSizeBefore = pathTaken->numParts();

    // A non-ok value for childElement will indicate that we need to append 'field' to the
    // 'pathToCreate' FieldRef.
    auto childElement = element->getDocument().end();
    invariant(!childElement.ok());
    if (!pathToCreate->empty()) {
        // We're already traversing a path with elements that don't exist yet, so we will definitely
        // need to append.
    } else if (element->getType() == BSONType::Object) {
        childElement = element->findFirstChildNamed(field);
    } else if (element->getType() == BSONType::Array) {
        boost::optional<size_t> indexFromField = parseUnsignedBase10Integer(field);
        if (indexFromField) {
            childElement = element->findNthChild(*indexFromField);
        } else {
            // We're trying to traverse an array element, but the path specifies a name instead of
            // an index. We append the name to 'pathToCreate' for now, even though we know we won't
            // be able to create it. If the update eventually needs to create the path,
            // pathsupport::createPathAt() will provide a sensible PathNotViable UserError.
        }
    }

    if (childElement.ok()) {
        // The path we've traversed so far already exists in our document, and 'childElement'
        // represents the Element indicated by the 'field' name or index, which we indicate by
        // updating the 'pathTaken' FieldRef.
        pathTaken->appendPart(field);
    } else {
        // We are traversing path components that do not exist in our document. Any update modifier
        // that creates new path components (i.e., any PathCreatingNode update nodes) will need to
        // create this component, so we append it to the 'pathToCreate' FieldRef.
        childElement = *element;
        pathToCreate->appendPart(field);
    }

    bool childAffectsIndexes = false;
    bool childNoop = false;

    child.apply(childElement,
                pathToCreate,
                pathTaken,
                matchedField,
                fromReplication,
                validateForStorage,
                immutablePaths,
                indexData,
                logBuilder,
                &childAffectsIndexes,
                &childNoop);

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
StatusWith<bool> UpdateObjectNode::parseAndMerge(
    UpdateObjectNode* root,
    modifiertable::ModifierType type,
    BSONElement modExpr,
    const CollatorInterface* collator,
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
                                    collator,
                                    arrayFilters,
                                    foundIdentifiers);
        if (!status.isOK()) {
            return status;
        }

        // 2) a RenameNode at the path specified by the value of the "modExpr" element, which must
        //    be a string value.
        if (BSONType::String != modExpr.type()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The 'to' field for $rename must be a string: "
                                        << modExpr);
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

    // Create UpdateInternalNodes along the path.
    UpdateInternalNode* current = static_cast<UpdateInternalNode*>(root);
    for (size_t i = 0; i < fieldRef.numParts() - 1; ++i) {
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
                                            << fieldRef.dottedSubstring(0, i + 1)
                                            << "'");
            }
        } else {
            std::unique_ptr<UpdateInternalNode> ownedChild;
            if (childShouldBeArrayNode) {
                ownedChild = stdx::make_unique<UpdateArrayNode>(arrayFilters);
            } else {
                ownedChild = stdx::make_unique<UpdateObjectNode>();
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
                      str::stream() << "Updating the path '" << fieldRef.dottedField()
                                    << "' would create a conflict at '"
                                    << fieldRef.dottedField()
                                    << "'");
    }
    current->setChild(std::move(childName), std::move(leaf));

    return positional;
}

// static
std::unique_ptr<UpdateNode> UpdateObjectNode::createUpdateNodeByMerging(
    const UpdateObjectNode& leftNode, const UpdateObjectNode& rightNode, FieldRef* pathTaken) {
    auto mergedNode = stdx::make_unique<UpdateObjectNode>();

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

void UpdateObjectNode::apply(mutablebson::Element element,
                             FieldRef* pathToCreate,
                             FieldRef* pathTaken,
                             StringData matchedField,
                             bool fromReplication,
                             bool validateForStorage,
                             const FieldRefSet& immutablePaths,
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
    auto applyChildClosure = [=, &element, &immutablePaths](const UpdateNode& child,
                                                            StringData field) {
        applyChild(child,
                   field,
                   &element,
                   pathToCreate,
                   pathTaken,
                   matchedField,
                   fromReplication,
                   validateForStorage,
                   immutablePaths,
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
}

}  // namespace mongo
