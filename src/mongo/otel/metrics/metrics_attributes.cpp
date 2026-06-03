/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/otel/metrics/metrics_attributes.h"

#include <algorithm>

#include <absl/container/flat_hash_set.h>

namespace mongo::otel::metrics {

#ifdef MONGO_CONFIG_OTEL
namespace {

// Converts an attribute value to its OpenTelemetry equivalent. The only type that actually needs
// conversion is StringData, as OpenTelemetry expects a std::string_view.
template <AttributeType T>
T toOtelAttributeValue(const T& value) noexcept {
    return value;
}
std::string_view toOtelAttributeValue(StringData value) noexcept {
    return toStdStringViewForInterop(value);
}
std::vector<std::string_view> toOtelAttributeValue(const std::span<StringData>& value) noexcept {
    std::vector<std::string_view> stringViews;
    stringViews.reserve(value.size());
    for (const StringData& s : value) {
        stringViews.push_back(toStdStringViewForInterop(s));
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
                // Because StringData isn't implicitly convertible to std::string_view, we need
                // to convert StringData and std::span<StringData> before calling the callback,
                // and create a temporary vector to hold the values converted from
                // std::span<StringData> until the callback is done.
                return callback(toStdStringViewForInterop(attr.name), toOtelAttributeValue(v));
            },
            attr.value);
    });
}
#endif  // MONGO_CONFIG_OTEL

bool containsDuplicates(std::span<const std::string> values) {
    absl::flat_hash_set<StringData> seenValues;
    for (StringData value : values) {
        if (!seenValues.insert(value).second) {
            return true;
        }
    }
    return false;
}

}  // namespace mongo::otel::metrics
