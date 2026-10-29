// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracing_utils.h"

#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
namespace otel {

OtelStringView asOtelStringView(std::string_view data) {
    return OtelStringView{data.data(), data.length()};
}

std::string_view asStringData(OtelStringView view) {
    return std::string_view{view.data(), view.length()};
}

stdx::unordered_set<OtelStringView> getKeySet(const TextMapCarrier& carrier) {
    stdx::unordered_set<OtelStringView> keys;
    bool visitedAll = carrier.Keys([&keys](OtelStringView key) {
        keys.insert(key);
        return true;
    });
    tassert(11029600,
            "TextMapCarrier failed to visit all keys despite callback always returning true",
            visitedAll);
    return keys;
}

}  // namespace otel
}  // namespace mongo
