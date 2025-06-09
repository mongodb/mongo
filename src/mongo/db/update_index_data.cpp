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

#include "mongo/db/update_index_data.h"

#include <cstddef>

#include <absl/container/btree_set.h>
#include <boost/container/small_vector.hpp>

// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"

namespace mongo {

UpdateIndexData::UpdateIndexData() : _allPathsIndexed(false) {}

void UpdateIndexData::addPath(const FieldRef& path) {
    _canonicalPaths.insert(FieldRef::getCanonicalIndexField(path));
}

void UpdateIndexData::addPathComponent(StringData pathComponent) {
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
