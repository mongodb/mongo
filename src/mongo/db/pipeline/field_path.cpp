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

#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {
const StringDataSet kAllowedDollarPrefixedFields = {
    // For DBRef
    "$id"_sd,
    "$ref"_sd,
    "$db"_sd,

    // Metadata fields.

    // This is necessary for sharded query execution of find() commands. mongos may attach a
    // $sortKey field to the projection sent to shards so that it can merge the results correctly.
    "$sortKey",

    // This is necessary for the "showRecordId" feature.
    "$recordId",

    // This is necessary for $search queries with a specified sort.
    "$searchSortValues"_sd,
    "$searchScore"_sd,
};

}  // namespace

using std::string;
using std::vector;

string FieldPath::getFullyQualifiedPath(StringData prefix, StringData suffix) {
    if (prefix.empty()) {
        return suffix.toString();
    }

    return str::stream() << prefix << "." << suffix;
}

FieldPath::FieldPath(std::string inputPath, bool precomputeHashes)
    : _fieldPath(std::move(inputPath)), _fieldPathDotPosition{string::npos} {
    uassert(40352, "FieldPath cannot be constructed with empty string", !_fieldPath.empty());
    uassert(40353, "FieldPath must not end with a '.'.", _fieldPath[_fieldPath.size() - 1] != '.');

    // Store index delimiter position for use in field lookup.
    size_t dotPos;
    size_t startPos = 0;
    while (string::npos != (dotPos = _fieldPath.find('.', startPos))) {
        _fieldPathDotPosition.push_back(dotPos);
        startPos = dotPos + 1;
    }

    _fieldPathDotPosition.push_back(_fieldPath.size());

    // Validate the path length and the fields, and precompute their hashes if requested.
    const auto pathLength = getPathLength();
    uassert(ErrorCodes::Overflow,
            "FieldPath is too long",
            pathLength <= BSONDepth::getMaxAllowableDepth());
    _fieldHash.reserve(pathLength);
    for (size_t i = 0; i < pathLength; ++i) {
        const auto& fieldName = getFieldName(i);
        uassertValidFieldName(fieldName);
        _fieldHash.push_back(precomputeHashes ? FieldNameHasher()(fieldName) : kHashUninitialized);
    }
}

void FieldPath::uassertValidFieldName(StringData fieldName) {
    uassert(15998, "FieldPath field names may not be empty strings.", !fieldName.empty());

    const auto dotsAndDollarsHint = " Consider using $getField or $setField.";

    if (fieldName[0] == '$' && !kAllowedDollarPrefixedFields.count(fieldName)) {
        uasserted(16410,
                  str::stream() << "FieldPath field names may not start with '$'."
                                << dotsAndDollarsHint);
    }

    uassert(
        16411, "FieldPath field names may not contain '\0'.", fieldName.find('\0') == string::npos);

    uassert(16412,
            str::stream() << "FieldPath field names may not contain '.'." << dotsAndDollarsHint,
            fieldName.find('.') == string::npos);
}

FieldPath FieldPath::concat(const FieldPath& tail) const {
    const FieldPath& head = *this;

    std::string concat;
    uassert(ErrorCodes::Overflow,
            "FieldPath concat would be too long",
            getPathLength() + tail.getPathLength() <= BSONDepth::getMaxAllowableDepth());
    const auto expectedStringSize = _fieldPath.size() + 1 + tail._fieldPath.size();
    concat.reserve(expectedStringSize);
    concat.insert(concat.begin(), head._fieldPath.begin(), head._fieldPath.end());
    concat.push_back('.');
    concat.insert(concat.end(), tail._fieldPath.begin(), tail._fieldPath.end());
    invariant(concat.size() == expectedStringSize);

    std::vector<size_t> newDots;
    // Subtract 2 since both contain std::string::npos at the beginning and the entire size at
    // the end. Add one because we inserted a dot in order to concatenate the two paths.
    const auto expectedDotSize =
        head._fieldPathDotPosition.size() + tail._fieldPathDotPosition.size() - 2 + 1;
    newDots.reserve(expectedDotSize);

    std::vector<size_t> newHashes;
    // We don't need the extra entry in hashes.
    newHashes.reserve(expectedDotSize - 1);

    // The first one in head._fieldPathDotPosition is npos. The last one, is, conveniently, the
    // size of head fieldPath, which also happens to be the index at which we added a new dot.
    newDots.insert(
        newDots.begin(), head._fieldPathDotPosition.begin(), head._fieldPathDotPosition.end());
    newHashes.insert(newHashes.begin(), head._fieldHash.begin(), head._fieldHash.end());

    invariant(tail._fieldPathDotPosition.size() >= 2);
    for (size_t i = 1; i < tail._fieldPathDotPosition.size(); ++i) {
        // Move each index back by size of the first field path, plus one, for the newly added dot.
        newDots.push_back(tail._fieldPathDotPosition[i] + head._fieldPath.size() + 1);
        newHashes.push_back(tail._fieldHash[i - 1]);
    }
    invariant(newDots.back() == concat.size());
    invariant(newDots.size() == expectedDotSize);
    invariant(newHashes.size() == expectedDotSize - 1);

    return FieldPath(std::move(concat), std::move(newDots), std::move(newHashes));
}
}  // namespace mongo
