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
/**
 * This library contains general types and functions used for supporting attributes in metrics.
 */
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <span>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>
#include <fmt/format.h>
#include <fmt/ranges.h>


namespace mongo::otel::metrics {

/**
 * The maximum number of attribute combinations allowed per metric. Attribute combinations increase
 * the memory footprint of metrics, the memory and storage of external consumers of metrics, and
 * possibly the time to record (due to less cache locality), so we set a conservative limit.
 */
constexpr int64_t kMaxAttributeCombinationsPerMetric = 1'000;

template <typename... Ts>
struct TypeList {};

/**
 * Possible types for attribute values. This is roughly the types supported in
 * `src/third_party/opentelemetry-cpp/api/include/opentelemetry/common/attribute_value.h`, with
 * some exceptions:
 * - No unsigned ints because only uint32_t is supported (not uint64_t or other unsigned types)
 * - StringData instead of std::string_view or char* because StringData is used in mongo code rather
 *   than std::string_view and it can replace char*.
 */
using AttributeTypes = TypeList<bool,
                                int32_t,
                                int64_t,
                                double,
                                StringData,
                                std::span<bool>,
                                std::span<int32_t>,
                                std::span<int64_t>,
                                std::span<double>,
                                std::span<StringData>>;

template <typename T, typename TList>
struct IsInList;
template <typename T, typename... Ts>
struct IsInList<T, TypeList<Ts...>> : std::bool_constant<(std::same_as<T, Ts> || ...)> {};
template <typename T>
/**
 * Concept for a supported attribute type.
 */
concept AttributeType = IsInList<T, AttributeTypes>::value;

/**
 * The definition of an attribute.
 */
template <AttributeType T>
struct MONGO_MOD_PUBLIC AttributeDefinition {
    std::string name;
    /**
     * All of the possible values this attribute can take.
     */
    std::vector<T> values;
};

template <typename T>
struct ToVariant;
template <typename... Ts>
struct ToVariant<TypeList<Ts...>> {
    using type = std::variant<Ts...>;
};
/**
 * A variant of all supported attribute types.
 */
using AnyAttributeType = ToVariant<AttributeTypes>::type;

/**
 * Holds an attribute name/value pair in a very space-efficient format.
 */
struct AttributeNameAndValue {
    StringData name;
    AnyAttributeType value;
};
template <typename T>
struct AttributesAndValue {
    std::vector<AttributeNameAndValue> attributes;
    T value;
};
/**
 * Sets of attributes and the value that they correspond to.
 */
template <typename T>
using AttributesAndValues = std::vector<AttributesAndValue<T>>;

/** Hash and equals for absl hash containers for tuples of attribute values. */
struct AttributesHasher;
struct AttributesEq;
/** Map keyed by a tuple of attribute values. */
template <typename KeyT, typename ValueT>
using AttributesMap = absl::flat_hash_map<KeyT, ValueT, AttributesHasher, AttributesEq>;

/**
 * Safely creates tuples that are the cartesian product of the provided values. This is "safe"
 * in that it throws BadValue exceptions if
 * - the number of resulting tuples is more than kMaxAttributeCombinationsPerMetric
 * - any attribute value for an attribute is duplicated
 * e.g. cartesianProduct({"a", "b"}, {1, 2}, {true, false}) yields the tuples
 *   {"a", 1, true}
 *   {"a", 1, false}
 *   {"a", 2, true}
 *   {"a", 2, false}
 *   {"b", 1, true}
 *   {"b", 1, false}
 *   {"b", 2, true}
 *   {"b", 2, false}
 */
template <typename... Ts>
std::vector<std::tuple<Ts...>> safeMakeAttributeTuples(const std::vector<Ts>&... values);

/**
 * Returns true if `values` contains duplicates.
 */
bool containsDuplicates(std::span<const std::string> values);

/* Returns true if two attribute values are logically equal. */
template <AttributeType T>
bool attributeValuesEqual(const T& a, const T& b);

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <AttributeType T>
bool attributeValuesEqual(const T& a, const T& b) {
    return a == b;
}
// std::span is not equality-comparable in C++20, so it requires a special case.
template <AttributeType T>
bool attributeValuesEqual(std::span<T> a, std::span<T> b) {
    return std::ranges::equal(a, b);
}

/** A small trait to detect std::span at compile-time */
template <typename T>
struct is_std_span : std::false_type {};

template <typename T, std::size_t Extent>
struct is_std_span<std::span<T, Extent>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_span_v = is_std_span<std::decay_t<T>>::value;
/** Wraps a single attribute value for use with absl::HashOf, converting std::span to absl::Span. */
template <AttributeType T>
MONGO_MOD_FILE_PRIVATE auto wrapForAbslHash(const T& v) {
    if constexpr (is_std_span_v<T>) {
        return absl::Span(v.data(), v.size());
    } else {
        return v;
    }
}

/** Hashes a single attribute value, with special handling for std::span. */
struct AttributeHasher {
    template <AttributeType T>
    size_t operator()(const T& v) const {
        return absl::HashOf(wrapForAbslHash(v));
    }
};

/** Equality for a single attribute value. */
struct AttributeEq {
    template <AttributeType T>
    bool operator()(const T& lhs, const T& rhs) const {
        return attributeValuesEqual(lhs, rhs);
    }
};

/** Hashes a tuple of attribute values. */
struct AttributesHasher {
    template <typename... Ts>
    size_t operator()(const std::tuple<Ts...>& t) const {
        return std::apply(
            [](const auto&... args) { return absl::HashOf(wrapForAbslHash(args)...); }, t);
    }
};

/** Equality for tuples of attribute values. */
struct AttributesEq {
    template <typename... Ts>
    bool operator()(const std::tuple<Ts...>& lhs, const std::tuple<Ts...>& rhs) const {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (AttributeEq{}(std::get<Is>(lhs), std::get<Is>(rhs)) && ...);
        }(std::index_sequence_for<Ts...>{});
    }
};

/**
 * Turns an attribute value into a string. This is needed because fmt::format doesn't support
 * std::span. This is only used for reporting errors.
 */
template <AttributeType T>
std::string formatAttributeValue(const T& v) {
    if constexpr (requires { fmt::format("{}", v); }) {
        return fmt::format("{}", v);
    } else {
        return fmt::format("[{}]", fmt::join(v, ", "));
    }
}

/**
 * Creates tuples that are the cartesian product of the provided values. Throws an error if any list
 * of values contains duplicates.
 * E.g. cartesianProduct({"a", "b"}, {1, 2}, {true, false}) yields the tuples
 *   {"a", 1, true}
 *   {"a", 1, false}
 *   {"a", 2, true}
 *   {"a", 2, false}
 *   {"b", 1, true}
 *   {"b", 1, false}
 *   {"b", 2, true}
 *   {"b", 2, false}
 */
MONGO_MOD_FILE_PRIVATE inline std::vector<std::tuple<>> cartesianProduct() {
    return {{}};
}
template <typename T, typename... Ts>
MONGO_MOD_FILE_PRIVATE std::vector<std::tuple<T, Ts...>> cartesianProduct(
    const std::vector<T>& values, const std::vector<Ts>&... rest) {
    std::vector<std::tuple<Ts...>> currentProduct = cartesianProduct(rest...);

    std::vector<std::tuple<T, Ts...>> result;
    result.reserve(values.size() * currentProduct.size());

    absl::flat_hash_set<T, AttributeHasher, AttributeEq> seenValues;
    for (const T& value : values) {
        massert(ErrorCodes::BadValue,
                fmt::format("Duplicate attribute value detected: {}", formatAttributeValue(value)),
                seenValues.insert(value).second);
        for (const std::tuple<Ts...>& currentTuple : currentProduct) {
            result.push_back(std::tuple_cat(std::make_tuple(value), currentTuple));
        }
    }
    return result;
}

template <typename... Ts>
std::vector<std::tuple<Ts...>> safeMakeAttributeTuples(const std::vector<Ts>&... values) {
    std::vector<std::tuple<Ts...>> result = cartesianProduct(values...);
    massert(
        ErrorCodes::BadValue,
        fmt::format("Attempted to create {} attribute combinations, which is more than the max {}",
                    result.size(),
                    kMaxAttributeCombinationsPerMetric),
        result.size() <= kMaxAttributeCombinationsPerMetric);
    return result;
}

}  // namespace mongo::otel::metrics
