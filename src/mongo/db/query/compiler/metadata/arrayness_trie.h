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

#pragma once

#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"

namespace mongo {

/**
 * Data structure representing arrayness of field paths.
 */
class PathArrayness {

    class TrieNode;

public:
    PathArrayness() {}

    ~PathArrayness() = default;

    /**
     * Insert a path into the trie.
     */
    void addPath(FieldPath path, MultikeyComponents multikeyPath);

    /**
     * Given a path return whether it is an array.
     */
    bool isPathArray(FieldPath path) const;

    /**
     * Debugging helper to visualize trie.
     */
    void visualizeTrie() const {
        _root->visualizeTrie();
    }

private:
    /**
     * Data structure representing the inner nodes of the trie.
     */
    class TrieNode {
    public:
        TrieNode(bool isArray = true) : _isArray(isArray) {}

        ~TrieNode() = default;

        /**
         * Insert a path with specific MultikeyComponents information.
         * The function is recursive and the "depth" parameter represents the specific component
         * inserted during a specific invocation.
         * e.g., path a.b.c, for depth 0 will insert 'a' for depth 1 will insert 'b' under 'a' and
         * depth 2 will insert 'c' under 'a.b'.
         */
        void insertPath(const FieldPath& path,
                        const MultikeyComponents& multikeyPath,
                        size_t depth);

        bool hasChildren() const {
            return _children.size();
        }

        bool isArray() const {
            return _isArray;
        }

        /**
         * Debugging helper to visualize trie.
         */
        void visualizeTrie(int depth = 0) const;

    private:
        /**
         * Child nodes representing further segments of the path.
         */
        std::map<std::string, TrieNode*> _children;

        /**
         * Represents whether the current node (i.e. path segment) may contain array values.
         * "true" indicates the path segment *may* contain array values.
         * "false" indicates that the path segment *definitely* does not contain array values.
         *
         * By default assume a field contains array value.
         */
        bool _isArray = true;
    };

    /**
     * The root to the trie.
     */
    TrieNode* _root = nullptr;
};

/**
 * Build an arrayness trie from a given vector of index entries.
 */
PathArrayness build(std::vector<IndexEntry> entries);

}  // namespace mongo
