// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/multikey_paths.h"

#include "mongo/db/field_ref.h"
#include "mongo/util/str.h"

#include <boost/container/small_vector.hpp>

namespace mongo::multikey_paths {

namespace {

// An index will fail to get created if the size in bytes of its key pattern is greater than 2048.
// We use that value to represent the largest number of path components we could ever possibly
// expect to see in an indexed field.
const size_t kMaxKeyPatternPathLength = 2048;

}  // namespace

std::string toString(const MultikeyPaths& paths) {
    str::stream builder;
    builder << "[";
    auto pathIt = paths.begin();
    while (true) {
        if (pathIt == paths.end()) {
            break;
        }
        builder << multikeyComponentsToString(*pathIt);
        if (++pathIt == paths.end()) {
            break;
        } else {
            builder << ",";
        }
    }
    builder << "]";
    return builder;
}

std::string multikeyComponentsToString(const MultikeyComponents& paths) {
    str::stream builder;
    builder << "{";
    auto pathIt = paths.begin();
    while (true) {
        if (pathIt == paths.end()) {
            break;
        }

        builder << *pathIt;
        if (++pathIt == paths.end()) {
            break;
        } else {
            builder << ",";
        }
    }
    builder << "}";
    return builder;
}

void serialize(const BSONObj& keyPattern, const MultikeyPaths& paths, BSONObjBuilder& builder) {
    char multikeyPathsEncodedAsBytes[kMaxKeyPatternPathLength];

    size_t i = 0;
    for (auto&& [fieldName, elem] : keyPattern) {
        size_t numParts = FieldRef{fieldName}.numParts();
        invariant(numParts > 0);
        invariant(numParts <= kMaxKeyPatternPathLength);

        std::fill_n(multikeyPathsEncodedAsBytes, numParts, 0);
        for (const auto multikeyComponent : paths[i]) {
            multikeyPathsEncodedAsBytes[multikeyComponent] = 1;
        }
        builder.appendBinData(fieldName, numParts, BinDataGeneral, &multikeyPathsEncodedAsBytes[0]);

        ++i;
    }
}

BSONObj serialize(const BSONObj& keyPattern, const MultikeyPaths& paths) {
    BSONObjBuilder builder;
    serialize(keyPattern, paths, builder);
    return builder.obj();
}

StatusWith<MultikeyPaths> parse(const BSONObj& obj) {
    MultikeyPaths paths;
    for (auto&& elem : obj) {
        if (elem.type() != BSONType::binData) {
            return {ErrorCodes::BadValue, "Multikey paths must be BinData"};
        }

        MultikeyComponents multikeyComponents;
        int len;
        const char* data = elem.binData(len);
        invariant(len > 0);

        if (static_cast<size_t>(len) > kMaxKeyPatternPathLength) {
            return {ErrorCodes::BadValue,
                    fmt::format("Multikey paths length cannot be greater than {}",
                                kMaxKeyPatternPathLength)};
        }

        for (int i = 0; i < len; ++i) {
            if (data[i]) {
                multikeyComponents.insert(i);
            }
        }
        paths.push_back(multikeyComponents);
    }
    return paths;
}

}  // namespace mongo::multikey_paths
