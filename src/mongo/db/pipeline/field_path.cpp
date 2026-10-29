// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/field_path.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
const StringDataSet kAllowedDollarPrefixedFields = {
    // For DBRef
    "$id"sv,
    "$ref"sv,
    "$db"sv,

    // Metadata fields.

    // This is necessary for sharded query execution of find() commands. mongos may attach a
    // $sortKey field to the projection sent to shards so that it can merge the results correctly.
    "$sortKey",

    // This is necessary for the "showRecordId" feature.
    "$recordId",

    // This is necessary for $search queries with a specified sort.
    "$searchSortValues"sv,
    "$searchScore"sv,
    "$searchRootDocumentId"sv,
};

}  // namespace

std::string FieldPath::getFullyQualifiedPath(std::string_view prefix, std::string_view suffix) {
    if (prefix.empty()) {
        return std::string{suffix};
    }

    return str::stream() << prefix << "." << suffix;
}

FieldPath::FieldPath(std::string inputPath, bool precomputeHashes, bool validateFieldNames) {
    auto statusWithFieldPath =
        fieldPathWithValidationStatus(std::move(inputPath), precomputeHashes, validateFieldNames);

    // TODO SERVER-117655: Refactor to avoid assigning to *this
    *this = uassertStatusOK(std::move(statusWithFieldPath));
}

StatusWith<FieldPath> fieldPathWithValidationStatus(std::string inputPath,
                                                    bool precomputeHashes,
                                                    bool validateFieldNames) {
    std::vector<size_t> dotPositions = {std::string::npos};

    if (inputPath.empty()) {
        return Status(ErrorCodes::Error{40352},
                      "FieldPath cannot be constructed with empty string");
    }

    if (inputPath.ends_with('.')) {
        return Status(ErrorCodes::Error{40353}, "FieldPath must not end with a '.'.");
    }

    // Store index delimiter position for use in field lookup.
    size_t dotPos;
    size_t startPos = 0;
    while (std::string::npos != (dotPos = inputPath.find('.', startPos))) {
        dotPositions.push_back(dotPos);
        startPos = dotPos + 1;
    }

    dotPositions.push_back(inputPath.size());

    // Validate the path length and the fields, and precompute their hashes if requested.
    const auto pathLength = FieldPath::getPathLength(dotPositions);
    if (pathLength > BSONDepth::getMaxAllowableDepth()) {
        return Status(ErrorCodes::Overflow, "FieldPath is too long");
    }

    // Only allocate heap storage space for the vector of precomputed hashes if it is actually
    // needed.
    std::vector<uint32_t> fieldHash;
    if (precomputeHashes) {
        fieldHash.reserve(pathLength);
    }
    Status validationStatus = Status::OK();
    for (size_t i = 0; i < pathLength; ++i) {
        auto fieldName = FieldPath::getFieldName(i, dotPositions, inputPath);
        if (validateFieldNames) {
            validationStatus = FieldPath::validateFieldName(fieldName);
            if (!validationStatus.isOK()) {
                return validationStatus.withContext(
                    "Consider using $getField or $setField for a field path with '.' or '$'.");
            }
        }
        if (precomputeHashes) {
            fieldHash.push_back(FieldNameHasher()(fieldName));
        }
    }

    return {FieldPath(std::move(inputPath), std::move(dotPositions), std::move(fieldHash))};
}

Status FieldPath::validateFieldName(std::string_view fieldName) {
    if (fieldName.empty()) {
        return Status(ErrorCodes::Error{15998}, "FieldPath field names may not be empty strings.");
    }

    if (fieldName[0] == '$' && !kAllowedDollarPrefixedFields.count(fieldName)) {
        return Status(ErrorCodes::Error{16410},
                      str::stream() << "FieldPath field names may not start with '$', given '"
                                    << fieldName << "'.");
    }

    if (fieldName.find('\0') != std::string::npos) {
        return Status(ErrorCodes::Error{16411},
                      str::stream() << "FieldPath field names may not contain '\0', given '"
                                    << fieldName << "'.");
    }

    if (fieldName.find('.') != std::string::npos) {
        return Status(ErrorCodes::Error{16412},
                      str::stream() << "FieldPath field names may not contain '.', given '"
                                    << fieldName << "'.");
    }

    return Status::OK();
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

    // The first one in head._fieldPathDotPosition is npos. The last one, is, conveniently, the
    // size of head fieldPath, which also happens to be the index at which we added a new dot.
    newDots.insert(
        newDots.begin(), head._fieldPathDotPosition.begin(), head._fieldPathDotPosition.end());

    invariant(tail._fieldPathDotPosition.size() >= 2);
    for (size_t i = 1; i < tail._fieldPathDotPosition.size(); ++i) {
        // Move each index back by size of the first field path, plus one, for the newly added dot.
        newDots.push_back(tail._fieldPathDotPosition[i] + head._fieldPath.size() + 1);
    }
    invariant(newDots.back() == concat.size());
    invariant(newDots.size() == expectedDotSize);

    // Re-use/compute the field hashes.
    // Field hashes are only needed if either 'head' or 'tail' has computed hashes. If neither have
    // computed hashes, we can skip copying them entirely.
    std::vector<uint32_t> newHashes;
    if (!head._fieldHash.empty() || !tail._fieldHash.empty()) {
        newHashes.reserve(head.getPathLength() + tail._fieldHash.size());

        // Insert all hashes from 'head' first. These may be zero hashes if hash computation hasn't
        // happened for 'head' yet.
        newHashes.insert(newHashes.end(), head._fieldHash.begin(), head._fieldHash.end());

        // Fill up any potential gap in the hashes for 'head'.
        for (size_t i = head._fieldHash.size(); i < head.getPathLength(); ++i) {
            newHashes.push_back(FieldNameHasher()(head.getFieldName(i)));
        }

        dassert(newHashes.size() == head.getPathLength());

        // Now append all hashes from 'tail'. These may be empty, so the hash values may only be
        // partially filled.
        newHashes.insert(newHashes.end(), tail._fieldHash.begin(), tail._fieldHash.end());
    }

    return FieldPath(std::move(concat), std::move(newDots), std::move(newHashes));
}
}  // namespace mongo
