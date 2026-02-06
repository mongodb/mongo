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

#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/util/modules.h"

namespace mongo {

class ExpressionContext;

/**
 * Data structure representing arrayness of field paths.
 */
class PathArrayness {

    class TrieNode;

public:
    PathArrayness() {}

    ~PathArrayness() = default;

    /**
     * Returns a reference to an empty PathArrayness instance. This represents the conservative
     * case: all field paths are treated as potentially containing arrays. This empty case may be
     * encountered when no Collection acquisition is possible (e.g. when running on a 'mongos').
     *
     * The returned reference is backed by a function-local static instance of PathArrayness.
     */
    static const PathArrayness& emptyPathArrayness();

    /**
     * Insert a path into the trie.
     */
    void addPath(const FieldPath& path, const MultikeyComponents& multikeyPath, bool isFullRebuild);

    /**
     * Insert all paths from an index into the trie. This method assumes 'multikeyPaths' is not
     * empty.
     * The parameter 'isFullRebuild' should be set by the caller if we are building the whole trie
     * from the full set of indexes.
     */
    void addPathsFromIndexKeyPattern(const BSONObj& indexKeyPattern,
                                     const MultikeyPaths& multikeyPaths,
                                     bool isFullRebuild);

    /**
     * Given a path return whether any component of it could be an array.
     * For field paths that are not included in any index, assumes that the path has an array.
     */
    bool canPathBeArray(const FieldPath& path, const ExpressionContext* expCtx) const;
    bool canPathBeArray(const FieldRef& path, const ExpressionContext* expCtx) const;

    /**
     * Debugging helper to visualize trie.
     */
    void visualizeTrie_forTest() const {
        _root.visualizeTrie_forTest("");
    }

    /**
     * Export the trie data structure to a mapping of field path prefixes to a boolean that
     * specifies whether or not that field path prefix is an array component.
     */
    stdx::unordered_map<std::string, bool> exportToMap_forTest();

    /**
     * Static helper to determine if an index is suitable for tracking array pathness.
     * We generally skip:
     * 1. Wildcard indexes: Unbounded size of multikey paths. We read later once we have
     * materialized 'CanonicalQuery'.
     * 2. Partial indexes: Don't cover the full document set.
     * 3. Hidden indexes: Intended to be invisible to the query optimizer.
     */
    static bool isIndexEligibleToAddToPathArrayness(const IndexDescriptor& descriptor);

private:
    /**
     * Data structure representing the inner nodes of the trie.
     */
    class TrieNode {
    public:
        TrieNode(bool canBeArray = true) : _canBeArray(canBeArray) {}

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
                        size_t depth,
                        bool isFullRebuild);

        bool hasChildren() const {
            return _children.size();
        }

        bool canBeArray() const {
            return _canBeArray;
        }

        /**
         * Helper function to determine whether any component of a given path could be an array.
         */
        bool canPathBeArray(const FieldPath& path) const;

        /**
         * Debugging helper to visualize trie.
         */
        void visualizeTrie_forTest(std::string fieldName, int depth = 0) const;

        /**
         * Return the next components of this specific field path along with the corresponding
         * `TrieNodes`. This is used only for debugging/testing purposes.
         */
        std::vector<std::pair<std::string, TrieNode>> getChildren() const {
            std::vector<std::pair<std::string, TrieNode>> childrenVector;
            std::copy(_children.begin(), _children.end(), std::back_inserter(childrenVector));
            return childrenVector;
        }

    private:
        /**
         * Child nodes representing further segments of the path.
         */
        // NOLINT is included to permit usage of std:: instead of stdx::. This is necessary due to
        // stricter compilation requirements on Windows variants.
        std::unordered_map<std::string, TrieNode> _children;  // NOLINT

        /**
         * Represents whether the current node (i.e. path segment) may contain array values.
         * "true" indicates the path segment *may* contain array values.
         * "false" indicates that the path segment *definitely* does not contain array values.
         *
         * By default assume a field contains array value.
         */
        bool _canBeArray = true;
    };

    /**
     * The root to the trie.
     */
    TrieNode _root;
};
}  // namespace mongo
