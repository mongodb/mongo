// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update_index_data.h"

#include <cstddef>
#include <string_view>

#include <absl/container/btree_set.h>

// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"

namespace mongo {

UpdateIndexData::UpdateIndexData() : _allPathsIndexed(false) {}

void UpdateIndexData::addPath(const FieldRef& path) {
    _canonicalPaths.insert(FieldRef::getCanonicalIndexField(path));
}

void UpdateIndexData::addPathComponent(std::string_view pathComponent) {
    _pathComponents.insert(std::string{pathComponent});
}

void UpdateIndexData::setAllPathsIndexed() {
    _allPathsIndexed = true;
}

bool UpdateIndexData::allPathsIndexed() const {
    return _allPathsIndexed;
}

void UpdateIndexData::clear() {
    _canonicalPaths.clear();
    _pathComponents.clear();
    _allPathsIndexed = false;
}

bool UpdateIndexData::isEmpty() const {
    return !_allPathsIndexed && _canonicalPaths.empty() && _pathComponents.empty();
}

bool UpdateIndexData::mightBeIndexed(const FieldRef& path) const {
    if (_allPathsIndexed) {
        return true;
    }

    for (const auto& indexedPath : _canonicalPaths) {
        if (FieldRef::pathOverlaps(path, indexedPath)) {
            return true;
        }
    }

    for (const auto& pathComponent : _pathComponents) {
        for (size_t partIdx = 0; partIdx < path.numParts(); ++partIdx) {
            if (pathComponent == path.getPart(partIdx)) {
                return true;
            }
        }
    }

    return false;
}

const absl::btree_set<FieldRef>& UpdateIndexData::getCanonicalPaths() const {
    return _canonicalPaths;
}

const absl::btree_set<std::string>& UpdateIndexData::getPathComponents() const {
    return _pathComponents;
}

}  // namespace mongo
