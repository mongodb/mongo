// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/otel/metrics/metrics_attributes.h"

#include <algorithm>
#include <string_view>

#include <absl/container/flat_hash_set.h>

namespace mongo::otel::metrics {

#ifdef MONGO_CONFIG_OTEL
namespace {

// Converts an attribute value to its OpenTelemetry equivalent. The only type that actually needs
// conversion is std::string_view, as OpenTelemetry expects a std::string_view.
template <AttributeType T>
T toOtelAttributeValue(const T& value) noexcept {
    return value;
}
std::string_view toOtelAttributeValue(std::string_view value) noexcept {
    return value;
}
std::vector<std::string_view> toOtelAttributeValue(
    const std::span<std::string_view>& value) noexcept {
    std::vector<std::string_view> stringViews;
    stringViews.reserve(value.size());
    for (const std::string_view& s : value) {
        stringViews.push_back(s);
    }
    return stringViews;
}
}  // namespace

bool AttributesKeyValueIterable::ForEachKeyValue(
    opentelemetry::nostd::function_ref<bool(opentelemetry::nostd::string_view,
                                            opentelemetry::common::AttributeValue)> callback)
    const noexcept {
    return std::ranges::all_of(_attributes, [&callback](const AttributeNameAndValue& attr) {
        return std::visit(
            [&callback, &attr](const auto& v) noexcept -> bool {
                // Because std::string_view isn't implicitly convertible to std::string_view, we
                // need to convert std::string_view and std::span<std::string_view> before calling
                // the callback, and create a temporary vector to hold the values converted from
                // std::span<std::string_view> until the callback is done.
                return callback(attr.name, toOtelAttributeValue(v));
            },
            attr.value);
    });
}
#endif  // MONGO_CONFIG_OTEL

bool containsDuplicates(std::span<const std::string> values) {
    absl::flat_hash_set<std::string_view> seenValues;
    for (std::string_view value : values) {
        if (!seenValues.insert(value).second) {
            return true;
        }
    }
    return false;
}

}  // namespace mongo::otel::metrics
