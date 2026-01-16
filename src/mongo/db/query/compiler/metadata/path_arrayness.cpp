/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/metadata/path_arrayness.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/logv2/log.h"

#include <stack>

using namespace mongo::multikey_paths;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

const PathArrayness& PathArrayness::emptyPathArrayness() {
    static const PathArrayness kEmptyPathArrayness;
    return kEmptyPathArrayness;
}

bool PathArrayness::isIndexEligibleToAddToPathArrayness(const IndexDescriptor& descriptor) {
    return (descriptor.getIndexType() != INDEX_WILDCARD) && !descriptor.isPartial() &&
        !descriptor.hidden();
}

void PathArrayness::addPath(const FieldPath& path,
                            const MultikeyComponents& multikeyPath,
                            bool isFullRebuild) {
    _root.insertPath(path, multikeyPath, 0, isFullRebuild);
}

void PathArrayness::addPathsFromIndexKeyPattern(const BSONObj& indexKeyPattern,
                                                const MultikeyPaths& multikeyPaths,
                                                bool isFullRebuild) {
    if (indexKeyPattern.isEmpty()) {
        // No paths to add if indexKeyPattern is empty.
        return;
    } else {
        tassert(11480900,
                "multikeyPaths must be non-empty when indexKeyPattern is non-empty",
                !multikeyPaths.empty());
    }

    size_t indexCounter = 0;
    for (const auto& key : indexKeyPattern) {
        FieldPath path(key.fieldNameStringData());
        addPath(path, multikeyPaths[indexCounter], isFullRebuild);
        ++indexCounter;
    }
}

bool PathArrayness::isPathArray(const FieldPath& path, const ExpressionContext* expCtx) const {
    // If the PathArrayness query knob is disabled, conservatively return true.
    if (!expCtx->getQueryKnobConfiguration().getEnablePathArrayness()) {
        return true;
    }

    bool arrayness = _root.isPathArray(path);
    LOGV2_DEBUG(
        11467800, 5, "Checking path arrayness", "path"_attr = path, "isPathArray"_attr = arrayness);
    return arrayness;
}

bool PathArrayness::isPathArray(const FieldRef& path, const ExpressionContext* expCtx) const {
    // If the PathArrayness query knob is disabled, conservatively return true.
    if (!expCtx->getQueryKnobConfiguration().getEnablePathArrayness()) {
        return true;
    }

    StringData pathString = path.dottedField(0);
    StatusWith<FieldPath> maybeFieldPath = fieldPathWithValidationStatus(std::string(pathString));

    // If FieldPath validation fails, conservatively assume this path is an array.
    if (!maybeFieldPath.isOK()) {
        return true;
    }

    return isPathArray(maybeFieldPath.getValue(), expCtx);
}

bool PathArrayness::TrieNode::isPathArray(const FieldPath& path) const {
    const TrieNode* current = this;
    // Track the number of times we have seen an array prefix.
    for (size_t depth = 0; depth < path.getPathLength(); ++depth) {
        const auto pathSegment = std::string(path.getFieldName(depth));
        const auto& next = current->_children.find(pathSegment);
        if (next == current->_children.end()) {
            // Missing path, conservatively assume all components from this point on are arrays.
            return true;
        }
        current = &next->second;
        if (current->isArray()) {
            return true;
        }
    }
    return current->isArray();
}

stdx::unordered_map<std::string, bool> PathArrayness::exportToMap_forTest() {
    stdx::unordered_map<std::string, bool> result;

    std::stack<std::pair<PathArrayness::TrieNode, std::string>> myStack;

    myStack.push({this->_root, ""});

    while (!myStack.empty()) {
        const auto [currNode, currPathComponent] = myStack.top();
        myStack.pop();

        // Do not insert the root (which is the only record with an empty fieldname)
        if (!currPathComponent.empty()) {
            result[currPathComponent] = currNode.isArray();
        }

        for (auto&& [childNode, childPathComponent] : currNode.getChildren()) {
            std::string pathPrefix = (currPathComponent.empty() ? "" : currPathComponent + ".");
            pathPrefix += childNode;
            myStack.push({childPathComponent, pathPrefix});
        }
    }

    return result;
}

void PathArrayness::TrieNode::visualizeTrie_forTest(std::string fieldName, int depth) const {
    for (int i = 0; i < depth; ++i) {
        std::cout << "  ";
    }

    std::cout << fieldName << "(" << _isArray << ")" << std::endl;

    // Recursively print children
    for (auto it = _children.begin(); it != _children.end(); ++it) {
        it->second.visualizeTrie_forTest(it->first, depth + 1);
    }
}

void PathArrayness::TrieNode::insertPath(const FieldPath& path,
                                         const MultikeyComponents& multikeyPath,
                                         size_t depth,
                                         bool isFullRebuild) {
    if (depth >= path.getPathLength()) {
        return;
    }

    const auto& fieldNameToInsert = std::string(path.getFieldName(depth));
    const auto& maybeChild = _children.find(fieldNameToInsert);

    if (maybeChild == _children.end()) {
        // This path component does not already exist so create a new TrieNode and insert it.
        _children.insert({fieldNameToInsert, TrieNode(multikeyPath.count(depth))});
    } else {
        // This path component already exists in trie so resolve conflicts in arrayness information.
        if (isFullRebuild) {
            // If we're fully rebuilding the trie from the full set of indexes, we can assume that
            // if the same path shows up as multikey and non-multikey then the non-multikey index is
            // more up to date.
            maybeChild->second._isArray &= (multikeyPath.count(depth) > 0);
        } else {
            // Otherwise (if we're inserting a path due to an index catalog update from a document
            // write operation), we take the conservative approach, i.e. prefer multikey in case of
            // conflict.
            maybeChild->second._isArray |= (multikeyPath.count(depth) > 0);
        }
    }

    // Recursively invoke the remaining path.
    _children.at(fieldNameToInsert).insertPath(path, multikeyPath, ++depth, isFullRebuild);
}
}  // namespace mongo
