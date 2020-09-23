/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <numeric>
#include <string>
#include <vector>

#include "mongo/stdx/variant.h"

namespace mongo {

/**
 * A path occurring as a fieldname in a $project stage or a find project which indicates source/
 * destination object fields for inclusion/exclusion and destination fields for computed projection.
 * Of the syntactic form: "a" or "a.b.c".
 */
struct ProjectionPath {
    auto operator==(const ProjectionPath& other) const {
        return components == other.components;
    }
    auto operator!=(const ProjectionPath& other) const {
        return !(*this == other);
    }

    std::vector<std::string> components;
};

/**
 * A path occurring as a fieldname in a find project indicating predicate application to array
 * elements. Of the syntactic form: "a.$" or "a.b.c.$".
 */
struct PositionalProjectionPath {
    auto operator==(const PositionalProjectionPath& other) const {
        return components == other.components;
    }
    auto operator!=(const PositionalProjectionPath& other) const {
        return !(*this == other);
    }

    std::vector<std::string> components;
};

/**
 * A path occurring as a value in aggregation expressions acting as a field reference. Of the
 * syntactic form: "$a" or "$a.b.c".
 */
struct AggregationPath {
    auto operator==(const AggregationPath& other) const {
        return components == other.components;
    }
    auto operator!=(const AggregationPath& other) const {
        return !(*this == other);
    }

    std::vector<std::string> components;
};

/**
 * A path occurring as a value in aggregation expressions acting as variable access. Of the
 * syntactic form: "$$a" or "$$a.b.c".
 */
struct AggregationVariablePath {
    auto operator==(const AggregationVariablePath& other) const {
        return components == other.components;
    }
    auto operator!=(const AggregationVariablePath& other) const {
        return !(*this == other);
    }

    std::vector<std::string> components;
};

/**
 * A path occurring in a sort specification.
 */
struct SortPath {
    auto operator==(const SortPath& other) const {
        return components == other.components;
    }
    auto operator!=(const SortPath& other) const {
        return !(*this == other);
    }

    std::vector<std::string> components;
};

namespace path {

template <typename StringType>
inline auto vectorToString(const std::vector<StringType>& vector) {
    return std::accumulate(
        std::next(vector.cbegin()),
        vector.cend(),
        std::string{vector[0]},
        [](auto&& pathString, auto&& element) { return pathString + "." + element; });
}

template <typename PathType>
inline auto vectorToString(const PathType& path) {
    return vectorToString(path.components);
}

}  // namespace path

/**
 * A path in the fieldname position in input BSON syntax. Such as "a.b" in '{"a.b": ""}'.
 */
using FieldnamePath = stdx::variant<ProjectionPath, PositionalProjectionPath, SortPath>;

/**
 * A path in the value position in input BSON syntax. Such as "$a.b" in '{"": "$a.b"}'.
 */
using ValuePath = stdx::variant<AggregationPath, AggregationVariablePath>;

}  // namespace mongo
