/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/index/multikey_paths.h"

#include "mongo/db/field_ref.h"
#include "mongo/util/str.h"

#include <boost/container/flat_set.hpp>
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
