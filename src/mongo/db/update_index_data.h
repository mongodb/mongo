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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"

#include <string>

#include <absl/container/btree_set.h>

namespace mongo {

/**
 * Holds pre-processed index spec information to allow update to quickly determine if an update
 * can be applied as a delta to a document, or if the document must be re-indexed.
 */
class UpdateIndexData {
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
    void addPathComponent(StringData pathComponent);

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
