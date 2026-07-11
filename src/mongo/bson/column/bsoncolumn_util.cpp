// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/column/bsoncolumn_util.h"

#include "mongo/bson/column/bsonobj_traversal.h"

#include <string_view>

namespace mongo::bsoncolumn {

uint32_t numInterleavedStreams(const BSONObj& refObj, uint8_t control) {
    BSONType rootType =
        static_cast<char>(control) == bsoncolumn::kInterleavedStartArrayRootControlByte
        ? BSONType::array
        : BSONType::object;

    bool traverseIntoArrays =
        static_cast<char>(control) == bsoncolumn::kInterleavedStartControlByte ||
        static_cast<char>(control) == bsoncolumn::kInterleavedStartArrayRootControlByte;

    uint32_t num = 0;
    BSONObjTraversal t(
        traverseIntoArrays,
        rootType,
        [](std::string_view fieldName, const BSONObj& obj, BSONType type) { return true; },
        [&num](const BSONElement& elem) {
            ++num;
            return true;
        });
    t.traverse(refObj);
    uassert(ErrorCodes::InvalidBSONColumn,
            "Invalid BSONColumn interleaved reference object for control byte",
            num != 0);

    return num;
}

}  // namespace mongo::bsoncolumn
