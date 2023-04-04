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

#include <limits>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Utility class which represents a field path with nested paths separated by dots.
 */
class FieldPath {
public:
    /**
     * Throws a AssertionException if a field name does not pass validation.
     */
    static void uassertValidFieldName(StringData fieldName);

    /**
     * Concatenates 'prefix' and 'suffix' using dotted path notation. 'prefix' is allowed to be
     * empty.
     */
    static std::string getFullyQualifiedPath(StringData prefix, StringData suffix);

    /**
     * Returns the substring of 'path' until the first '.', or the entire string if there is no '.'.
     */
    static StringData extractFirstFieldFromDottedPath(StringData path) {
        return path.substr(0, path.find('.'));
    }

    /**
     * Throws a AssertionException if the string is empty or if any of the field names fail
     * validation.
     *
     * Field names are validated using uassertValidFieldName().
     */
    /* implicit */ FieldPath(std::string inputPath, bool precomputeHashes = false);
    /* implicit */ FieldPath(StringData inputPath, bool precomputeHashes = false)
        : FieldPath(inputPath.toString(), precomputeHashes) {}
    /* implicit */ FieldPath(const char* inputPath, bool precomputeHashes = false)
        : FieldPath(std::string(inputPath), precomputeHashes) {}

    /**
     * Returns the number of path elements in the field path.
     */
    size_t getPathLength() const {
        return _fieldPathDotPosition.size() - 1;
    }

    /**
     * Get the subpath including path elements [0, n].
     */
    StringData getSubpath(size_t n) const {
        invariant(n + 1 < _fieldPathDotPosition.size());
        return StringData(_fieldPath.c_str(), _fieldPathDotPosition[n + 1]);
    }

    /**
     * Return the first path component.
     */
    StringData front() const {
        return getFieldName(0);
    }

    /**
     * Return the last path component.
     */
    StringData back() const {
        return getFieldName(getPathLength() - 1);
    }

    /**
     * Return the ith field name from this path using zero-based indexes.
     */
    StringData getFieldName(size_t i) const {
        dassert(i < getPathLength());
        const auto begin = _fieldPathDotPosition[i] + 1;
        const auto end = _fieldPathDotPosition[i + 1];
        return StringData(&_fieldPath[begin], end - begin);
    }

    /**
     * Return the ith field name from this path using zero-based indexes, with pre-computed hash.
     */
    HashedFieldName getFieldNameHashed(size_t i) const {
        dassert(i < getPathLength());
        invariant(_fieldHash[i] != kHashUninitialized);
        return HashedFieldName{getFieldName(i), _fieldHash[i]};
    }

    /**
     * Returns the full path, not including the prefix 'FieldPath::prefix'.
     */
    const std::string& fullPath() const {
        return _fieldPath;
    }

    /**
     * Returns the full path, including the prefix 'FieldPath::prefix'.
     */
    std::string fullPathWithPrefix() const {
        return prefix + _fieldPath;
    }

    /**
     * A FieldPath like this but missing the first element (useful for recursion).
     * Precondition getPathLength() > 1.
     */
    FieldPath tail() const {
        massert(16409, "FieldPath::tail() called on single element path", getPathLength() > 1);
        return {_fieldPath.substr(_fieldPathDotPosition[1] + 1)};
    }

    /**
     * Returns a FieldPath like this, but missing the last element.
     */
    FieldPath withoutLastElement() const {
        return FieldPath(getSubpath(getPathLength() - 2));
    }

    FieldPath concat(const FieldPath& tail) const;

private:
    FieldPath(std::string string, std::vector<size_t> dots, std::vector<size_t> hashes)
        : _fieldPath(std::move(string)),
          _fieldPathDotPosition(std::move(dots)),
          _fieldHash(std::move(hashes)) {
        uassert(ErrorCodes::Overflow,
                "FieldPath is too long",
                _fieldPathDotPosition.size() <= BSONDepth::getMaxAllowableDepth());
    }

    static const char prefix = '$';

    // Contains the full field path, with each field delimited by a '.' character.
    std::string _fieldPath;

    // Contains the position of field delimiter dots in '_fieldPath'. The first element contains
    // string::npos (which evaluates to -1) and the last contains _fieldPath.size() to facilitate
    // lookup.
    std::vector<size_t> _fieldPathDotPosition;

    // Contains the hash value for the field names if it was requested when creating this path.
    // Otherwise all elements are set to 'kHashUninitialized'.
    std::vector<size_t> _fieldHash;
    static constexpr std::size_t kHashUninitialized = std::numeric_limits<std::size_t>::max();
};

inline bool operator<(const FieldPath& lhs, const FieldPath& rhs) {
    return lhs.fullPath() < rhs.fullPath();
}

inline bool operator==(const FieldPath& lhs, const FieldPath& rhs) {
    return lhs.fullPath() == rhs.fullPath();
}
}  // namespace mongo
