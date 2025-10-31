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

using namespace mongo::multikey_paths;

namespace mongo {

void PathArrayness::addPath(const FieldPath& path, const MultikeyComponents& multikeyPath) {
    _root.insertPath(path, multikeyPath, 0);
}

bool PathArrayness::isPathArray(const FieldPath& path) const {
    return _root.isPathArray(path);
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

void PathArrayness::TrieNode::visualizeTrie(std::string fieldName, int depth) const {
    for (int i = 0; i < depth; ++i) {
        std::cout << "  ";
    }

    std::cout << fieldName << "(" << _isArray << ")" << std::endl;

    // Recursively print children
    for (auto it = _children.begin(); it != _children.end(); ++it) {
        it->second.visualizeTrie(it->first, depth + 1);
    }
}

void PathArrayness::TrieNode::insertPath(const FieldPath& path,
                                         const MultikeyComponents& multikeyPath,
                                         size_t depth) {
    if (depth >= path.getPathLength()) {
        return;
    }

    // Insert the top level field.
    std::string fieldNameToInsert = std::string(path.getFieldName(depth));
    if (!_children.contains(fieldNameToInsert)) {
        _children.insert({fieldNameToInsert, TrieNode(multikeyPath.count(depth))});
    }

    // Recursively invoke the remaining path.
    _children.at(fieldNameToInsert).insertPath(path, multikeyPath, ++depth);
}

PathArrayness build(std::vector<IndexEntry> entries) {
    PathArrayness root;
    return root;
}

}  // namespace mongo
