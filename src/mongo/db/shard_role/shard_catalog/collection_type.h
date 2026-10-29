// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo::query_shape {
using namespace std::literals::string_view_literals;

// This enum is not compatible with the QUERY_UTIL_NAMED_ENUM_DEFINE util since the "virtual" type
// conflicts with the C++ keyword "virtual". Instead, we manually define the enum and the
// toStringData function below.
enum class CollectionType {
    kUnknown,
    kCollection,
    kView,
    kTimeseries,
    kChangeStream,
    kVirtual,
    kNonExistent,
};

static std::string_view toStringData(CollectionType type) {
    switch (type) {
        case CollectionType::kUnknown:
            return "unknown"sv;
        case CollectionType::kCollection:
            return "collection"sv;
        case CollectionType::kView:
            return "view"sv;
        case CollectionType::kTimeseries:
            return "timeseries"sv;
        case CollectionType::kChangeStream:
            return "changeStream"sv;
        case CollectionType::kVirtual:
            return "virtual"sv;
        case CollectionType::kNonExistent:
            return "nonExistent"sv;
        default:
            MONGO_UNREACHABLE_TASSERT(7804900);
    }
}

}  // namespace mongo::query_shape
