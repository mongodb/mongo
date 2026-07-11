// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/field_ref.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <absl/container/btree_set.h>

namespace mongo {

/**
 * Holds pre-processed index spec information to allow update to quickly determine if an update
 * can be applied as a delta to a document, or if the document must be re-indexed.
 */
class [[MONGO_MOD_PUBLIC]] UpdateIndexData {
public:
    UpdateIndexData();

    /**
     * Register a path.  Any update targeting this path (or a parent of this path) will
     * trigger a recomputation of the document's index keys.
     */
    void addPath(const FieldRef& path);

    /**
     * Register a path component.  Any update targeting a path that contains this exact
     * component will trigger a recomputation of the document's index keys.
     */
    void addPathComponent(std::string_view pathComponent);

    /**
     * Register the "wildcard" path for wildcard indexes or wildcard text indexes.
     * All updates of a document will trigger a recomputation of the document's index keys for this
     * index.
     */
    void setAllPathsIndexed();

    /**
     * Returns whether the "wildcard" path was registered before.
     */
    bool allPathsIndexed() const;

    void clear();

    bool mightBeIndexed(const FieldRef& path) const;

    /**
     * Return whether this structure has been cleared or has not been initialized yet.
     */
    bool isEmpty() const;

    /**
     * Get canonical path of each indexed field.
     */
    const absl::btree_set<FieldRef>& getCanonicalPaths() const;

    /**
     * Get all path components.
     * Path components contain the name(s) of the "language" field(s) in text indexes.
     */
    const absl::btree_set<std::string>& getPathComponents() const;

private:
    /**
     * Canonicalized paths of the index fields.
     */
    absl::btree_set<FieldRef> _canonicalPaths;

    /**
     * The name(s) of the "language" field(s) in text indexes, if any.
     */
    absl::btree_set<std::string> _pathComponents;

    /**
     * Set to true if the index is either a wildcard index ({"$**": 0|1}) or a wildcard text index
     * ({"$**": "text"}). In this case the index assumes responsibility for all fields.
     */
    bool _allPathsIndexed;
};
}  // namespace mongo
