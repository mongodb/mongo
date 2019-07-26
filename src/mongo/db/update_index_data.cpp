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
#include "mongo/bson/util/builder.h"

namespace mongo {

UpdateIndexData::UpdateIndexData() : _allPathsIndexed(false) {}

void UpdateIndexData::addPath(const FieldRef& path) {
    _canonicalPaths.insert(getCanonicalIndexField(path));
}

void UpdateIndexData::addPathComponent(StringData pathComponent) {
    _pathComponents.insert(pathComponent.toString());
}

void UpdateIndexData::allPathsIndexed() {
    _allPathsIndexed = true;
}

void UpdateIndexData::clear() {
    _canonicalPaths.clear();
    _pathComponents.clear();
    _allPathsIndexed = false;
}

bool UpdateIndexData::mightBeIndexed(const FieldRef& path) const {
    if (_allPathsIndexed) {
        return true;
    }

    FieldRef canonicalPath = getCanonicalIndexField(path);

    for (const auto& idx : _canonicalPaths) {
        if (_startsWith(canonicalPath, idx) || _startsWith(idx, canonicalPath))
            return true;
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

bool UpdateIndexData::_startsWith(const FieldRef& a, const FieldRef& b) const {
    return (a == b) || (b.isPrefixOf(a));
}

FieldRef UpdateIndexData::getCanonicalIndexField(const FieldRef& path) {
    if (path.numParts() <= 1)
        return path;

    // The first part of the path must always be a valid field name, since it's not possible to
    // store a top-level array or '$' field name in a document.
    FieldRef buf(path.getPart(0));
    for (size_t i = 1; i < path.numParts(); ++i) {
        auto pathComponent = path.getPart(i);

        if (pathComponent == "$"_sd) {
            continue;
        }

        if (FieldRef::isNumericPathComponentLenient(pathComponent)) {
            // Peek ahead to see if the next component is also all digits. This implies that the
            // update is attempting to create a numeric field name which would violate the
            // "ambiguous field name in array" constraint for multi-key indexes. Break early in this
            // case and conservatively return that this path affects the prefix of the consecutive
            // numerical path components. For instance, an input such as 'a.0.1.b.c' would return
            // the canonical index path of 'a'.
            if ((i + 1) < path.numParts() &&
                FieldRef::isNumericPathComponentLenient(path.getPart(i + 1))) {
                break;
            }
            continue;
        }

        buf.appendPart(pathComponent);
    }

    return buf;
}
}  // namespace mongo
