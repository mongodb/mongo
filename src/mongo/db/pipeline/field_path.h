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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/util/assert_util.h"

#include <compare>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

/**
 * Utility class which represents a field path with nested paths separated by dots.
 */
class FieldPath {
public:
    /**
     * Throws a AssertionException if a field name does not pass validation.
     */
    static Status validateFieldName(StringData fieldName);

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
     * Field names are validated using validateFieldName().
     */
    /* implicit */ FieldPath(std::string inputPath,
                             bool precomputeHashes = false,
                             bool validateFieldNames = true);
    /* implicit */ FieldPath(StringData inputPath,
                             bool precomputeHashes = false,
                             bool validateFieldNames = true)
        : FieldPath(std::string{inputPath}, precomputeHashes, validateFieldNames) {}
    /* implicit */ FieldPath(const char* inputPath,
                             bool precomputeHashes = false,
                             bool validateFieldNames = true)
        : FieldPath(std::string(inputPath), precomputeHashes, validateFieldNames) {}

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
     * Tasserts when trying to access a field name hash for a position for which no hashes were
     * calculated.
     */
    HashedFieldName getFieldNameHashed(size_t i) const {
        dassert(i < getPathLength());
        tassert(
            11212700, "cannot access a not-calculated field hash position", i < _fieldHash.size());
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
        tassert(16409, "FieldPath::tail() called on single element path", getPathLength() > 1);
        return {_fieldPath.substr(_fieldPathDotPosition[1] + 1)};
    }

    /**
     * Returns a FieldPath like this, but missing the last element.
     */
    FieldPath withoutLastElement() const {
        return FieldPath(getSubpath(getPathLength() - 2));
    }

    FieldPath concat(const FieldPath& tail) const;

    bool isPrefixOf(const FieldPath& rhsPath) const {
        const auto& lhsStr = fullPath();
        const auto& rhsStr = rhsPath.fullPath();
        return lhsStr.size() < rhsStr.size()
            ? rhsStr.starts_with(lhsStr) && rhsStr[lhsStr.size()] == '.'
            : lhsStr == rhsStr;
    }

    FieldPath subtractPrefix(size_t prefixLength) const {
        tassert(10985000,
                "Expected prefixLength < numPathElements",
                prefixLength + 1 < _fieldPathDotPosition.size());
        if (prefixLength == 0) {
            return *this;
        }
        return FieldPath(_fieldPath.substr(_fieldPathDotPosition[prefixLength] + 1));
    }

private:
    FieldPath(std::string string, std::vector<size_t> dots, std::vector<uint32_t> hashes)
        : _fieldPath(std::move(string)),
          _fieldPathDotPosition(std::move(dots)),
          _fieldHash(std::move(hashes)) {
        uassert(ErrorCodes::Overflow,
                "FieldPath is too long",
                _fieldPathDotPosition.size() <= BSONDepth::getMaxAllowableDepth());
    }

    static constexpr char prefix = '$';

    // Contains the full field path, with each field delimited by a '.' character.
    std::string _fieldPath;

    // Contains the position of field delimiter dots in '_fieldPath'. The first element contains
    // string::npos (which evaluates to -1) and the last contains _fieldPath.size() to facilitate
    // lookup.
    std::vector<size_t> _fieldPathDotPosition;

    // Contains the hash values for the field names, if it was requested when creating this path.
    // The hashes can be accessed via 'getFieldNameHashed(i)'.
    // Can be empty if precomputing hashes was not requested.
    // Can also be partially empty, with slots at the end not being fully filled, after
    // concatenating two field paths of which the second did not have precomputed hashes.
    std::vector<uint32_t> _fieldHash;
};

inline bool operator<(const FieldPath& lhs, const FieldPath& rhs) {
    return lhs.fullPath() < rhs.fullPath();
}

inline bool operator==(const FieldPath& lhs, const FieldPath& rhs) {
    return lhs.fullPath() == rhs.fullPath();
}

template <typename H>
H AbslHashValue(H h, const FieldPath& fieldPath) {
    return H::combine(std::move(h), fieldPath.fullPath());
}
}  // namespace mongo
