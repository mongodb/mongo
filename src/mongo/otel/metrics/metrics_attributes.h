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
#include "mongo/config.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/common/key_value_iterable.h>
#include <opentelemetry/nostd/function_ref.h>
#include <opentelemetry/nostd/string_view.h>
#endif


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

/**
 * Holds a collection of attribute name/value pairs and implements the opentelemetry
 * KeyValueIterable interface (when OTel is enabled) so it can be passed directly to OTel
 * observer callbacks.
 */
class AttributesKeyValueIterable
#ifdef MONGO_CONFIG_OTEL
    : public opentelemetry::common::KeyValueIterable
#endif
{
public:
    using value_type = AttributeNameAndValue;

    AttributesKeyValueIterable() = default;
    explicit AttributesKeyValueIterable(std::vector<AttributeNameAndValue> attributes)
        : _attributes(std::move(attributes)) {}

    /** Container interface, which allows it to be looped over and enables GoogleTest matchers. */
    auto begin() const noexcept {
        return _attributes.begin();
    }
    auto end() const noexcept {
        return _attributes.end();
    }
    bool empty() const noexcept {
        return _attributes.empty();
    }

#ifdef MONGO_CONFIG_OTEL
    bool ForEachKeyValue(
        opentelemetry::nostd::function_ref<bool(opentelemetry::nostd::string_view,
                                                opentelemetry::common::AttributeValue)> callback)
        const noexcept override;

    size_t size() const noexcept override {
        return _attributes.size();
    }
#else
    size_t size() const noexcept {
        return _attributes.size();
    }
#endif

private:
    std::vector<AttributeNameAndValue> _attributes;
};

template <typename T>
struct AttributesAndValue {
    AttributesKeyValueIterable attributes;
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
 * Maps view attribute types to their owned equivalents for safe internal storage.
 * Non-view types are unchanged.
 */
template <typename T>
struct AttributeOwnership {
    using OwnedType = T;
};
template <>
struct AttributeOwnership<StringData> {
    using OwnedType = std::string;
};
template <>
struct AttributeOwnership<std::span<StringData>> {
    struct OwnedType {
        std::vector<std::string> strings;
        std::vector<StringData> stringDatas;
    };
};
template <typename T>
struct AttributeOwnership<std::span<T>> {
    using OwnedType = std::vector<T>;
};
/**
 * std::vector<bool> is a special case in C++ that packs booleans as individual bits, so it lacks a
 * data() method and cannot back a std::span<bool>. This specialization uses a unique_ptr<bool[]>
 * for contiguous storage instead.
 */
template <>
struct AttributeOwnership<std::span<bool>> {
    struct OwnedType {
        size_t size;
        std::unique_ptr<bool[]> storage;
    };
};

/**
 * Per-attribute owned storage for view-type values (StringData → std::string,
 * std::span<T> → std::vector<T>). Each value is heap-allocated via unique_ptr so its address
 * is stable: StringData/span views into this storage remain valid even if the owning object moves.
 *
 * A named struct (rather than a type alias for std::tuple) so that template argument deduction
 * works when passing it to safeMakeAttributeTuples.
 */
template <AttributeType... AttributeTs>
struct OwnedAttributeValueLists {
    std::tuple<std::vector<std::unique_ptr<typename AttributeOwnership<AttributeTs>::OwnedType>>...>
        lists;
};

/**
 * Builds an OwnedAttributeValueLists from a set of AttributeDefinitions, heap-allocating owned
 * copies of each attribute value.
 */
template <AttributeType... AttributeTs>
OwnedAttributeValueLists<AttributeTs...> makeOwnedAttributeValueLists(
    const AttributeDefinition<AttributeTs>&... defs);

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
 * Overload of safeMakeAttributeTuples that accepts an OwnedAttributeValueLists and converts each
 * owned value to its view type before computing the cartesian product.
 */
template <AttributeType... AttributeTs>
std::vector<std::tuple<AttributeTs...>> safeMakeAttributeTuples(
    const OwnedAttributeValueLists<AttributeTs...>& ownedLists);

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
 * std::span.
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

/** Converts an owned attribute value back to its view type. */
template <AttributeType T>
MONGO_MOD_FILE_PRIVATE T toView(const T& owned) {
    return owned;
}
MONGO_MOD_FILE_PRIVATE inline StringData toView(const std::string& owned) {
    return owned;
}
// const_cast is safe: the data is owned and non-const; const is an artifact of the parameter.
MONGO_MOD_FILE_PRIVATE inline std::span<StringData> toView(
    const AttributeOwnership<std::span<StringData>>::OwnedType& owned) {
    return {const_cast<StringData*>(owned.stringDatas.data()), owned.stringDatas.size()};
}
// const_cast is safe: the data is owned and non-const; const is an artifact of map iteration.
template <typename T>
MONGO_MOD_FILE_PRIVATE std::span<T> toView(const std::vector<T>& owned) {
    return {const_cast<T*>(owned.data()), owned.size()};
}
MONGO_MOD_FILE_PRIVATE inline std::span<bool> toView(
    const AttributeOwnership<std::span<bool>>::OwnedType& owned) {
    return {owned.storage.get(), owned.size};
}

/** Converts a list of heap-allocated owned values to a vector of their view types. */
template <AttributeType ViewT>
MONGO_MOD_FILE_PRIVATE std::vector<ViewT> viewsOf(
    const std::vector<std::unique_ptr<typename AttributeOwnership<ViewT>::OwnedType>>& owned) {
    std::vector<ViewT> views;
    views.reserve(owned.size());
    for (const auto& ptr : owned)
        views.push_back(toView(*ptr));
    return views;
}

/** Converts a view attribute value to its owned equivalent. */
template <AttributeType T>
MONGO_MOD_FILE_PRIVATE T toOwned(const T& val) {
    return val;
}
MONGO_MOD_FILE_PRIVATE inline std::string toOwned(StringData val) {
    return std::string(val);
}
template <typename T>
MONGO_MOD_FILE_PRIVATE std::vector<T> toOwned(std::span<T> val) {
    return {val.begin(), val.end()};
}
MONGO_MOD_FILE_PRIVATE inline AttributeOwnership<std::span<StringData>>::OwnedType toOwned(
    const std::span<StringData>& val) {
    AttributeOwnership<std::span<StringData>>::OwnedType result{
        .strings = std::vector<std::string>(val.begin(), val.end())};
    result.stringDatas = std::vector<StringData>(result.strings.begin(), result.strings.end());
    return result;
}
MONGO_MOD_FILE_PRIVATE inline AttributeOwnership<std::span<bool>>::OwnedType toOwned(
    std::span<bool> val) {
    auto storage = std::make_unique<bool[]>(val.size());
    std::copy(val.begin(), val.end(), storage.get());
    return {.size = val.size(), .storage = std::move(storage)};
}

template <AttributeType... AttributeTs>
OwnedAttributeValueLists<AttributeTs...> makeOwnedAttributeValueLists(
    const AttributeDefinition<AttributeTs>&... defs) {
    return {.lists = std::make_tuple([](const auto& values) {
                // Use value_type rather than the iterator's dereference type to avoid
                // the std::vector<bool> proxy reference (std::__bit_const_reference).
                using ViewT = typename std::decay_t<decltype(values)>::value_type;
                using OwnedT = typename AttributeOwnership<ViewT>::OwnedType;
                std::vector<std::unique_ptr<OwnedT>> ptrs;
                ptrs.reserve(values.size());
                for (const auto& val : values)
                    ptrs.push_back(std::make_unique<OwnedT>(toOwned(static_cast<ViewT>(val))));
                return ptrs;
            }(defs.values)...)};
}

template <AttributeType... AttributeTs>
std::vector<std::tuple<AttributeTs...>> safeMakeAttributeTuples(
    const OwnedAttributeValueLists<AttributeTs...>& ownedLists) {
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return safeMakeAttributeTuples(viewsOf<AttributeTs>(std::get<Is>(ownedLists.lists))...);
    }(std::index_sequence_for<AttributeTs...>{});
}

}  // namespace mongo::otel::metrics
