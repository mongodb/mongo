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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/idl_parser.h"

#include <type_traits>
#include <variant>
#include <vector>

namespace mongo {

// std::monostate is a removal sentinel (null on wire, used during PQS merge).
// Enum-typed knobs are stored as int.
using QueryKnobValue = std::variant<std::monostate, int, long long, double, bool>;

using ReadGlobalFn = QueryKnobValue (*)();

template <typename S>
concept AtomicLoadable = requires(const S& s) {
    { s.load() };
};

template <typename S>
concept SynchronizedEnum = requires(const S& s) {
    { s.get() } -> std::convertible_to<typename S::value_type>;
    requires std::is_enum_v<typename S::value_type>;
};

/**
 * Concept-dispatched reader. The `auto&` NTTP produces one instantiation per global variable.
 * Enum values from synchronized_value<EnumType> are cast to int in the variant.
 */
template <auto& storage>
QueryKnobValue readGlobalValue() {
    using Storage = std::remove_cvref_t<decltype(storage)>;
    if constexpr (AtomicLoadable<Storage>) {
        return QueryKnobValue{storage.load()};
    } else if constexpr (SynchronizedEnum<Storage>) {
        return QueryKnobValue{static_cast<int>(storage.get())};
    } else {
        static_assert(sizeof(Storage) == 0, "Unsupported storage type for readGlobalValue");
    }
}

/**
 * Type-erased descriptor for a single query knob. Each typed QueryKnob<T> inherits from this and
 * self-registers into QueryKnobDescriptorSet at static init time. The index field is left as a
 * sentinel (~0) until QueryKnobRegistry assigns dense indices during startup.
 *
 * Non-copyable to prevent object slicing when used through the base pointer.
 */
struct QueryKnobBase {
    QueryKnobBase(StringData name, ReadGlobalFn readFn)
        : paramName(name), readGlobal(readFn), fromBSON(nullptr), toBSON(nullptr) {}
    virtual ~QueryKnobBase() = default;
    QueryKnobBase(const QueryKnobBase&) = delete;
    QueryKnobBase& operator=(const QueryKnobBase&) = delete;

    StringData paramName;
    size_t index = ~size_t{0};  // Sentinel until QueryKnobRegistry assigns a dense index.
    ReadGlobalFn readGlobal;
    QueryKnobValue (*fromBSON)(const BSONElement&);
    void (*toBSON)(BSONObjBuilder&, StringData, const QueryKnobValue&);
};

/**
 * Compile-time collection of all QueryKnob descriptors. Populated during static
 * init, read-only thereafter. QueryKnobRegistry consumes this to build its index.
 */
class QueryKnobDescriptorSet {
public:
    static QueryKnobDescriptorSet& get();

    // Non-const knob: QueryKnobRegistry will later assign a dense index into each registered knob.
    void add(QueryKnobBase& knob);
    const std::vector<QueryKnobBase*>& knobs() const;

private:
    std::vector<QueryKnobBase*> _knobs;
};

namespace detail {

template <typename T>
struct ConverterTraits {
    static_assert(sizeof(T) == 0, "Unsupported QueryKnob type");
};

template <typename T>
requires std::is_enum_v<T>
struct ConverterTraits<T> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        return static_cast<int>(idl::deserialize<T>(elem.str()));
    }
    static void toBSON(BSONObjBuilder& b, StringData field, const QueryKnobValue& val) {
        b.append(field, idl::serialize(static_cast<T>(std::get<int>(val))));
    }
};

template <>
struct ConverterTraits<int> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        return elem.Int();
    }
    static void toBSON(BSONObjBuilder& b, StringData field, const QueryKnobValue& val) {
        b.append(field, std::get<int>(val));
    }
};

template <>
struct ConverterTraits<long long> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        return elem.Long();
    }
    static void toBSON(BSONObjBuilder& b, StringData field, const QueryKnobValue& val) {
        b.append(field, std::get<long long>(val));
    }
};

template <>
struct ConverterTraits<double> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        return elem.Double();
    }
    static void toBSON(BSONObjBuilder& b, StringData field, const QueryKnobValue& val) {
        b.append(field, std::get<double>(val));
    }
};

template <>
struct ConverterTraits<bool> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        return elem.Bool();
    }
    static void toBSON(BSONObjBuilder& b, StringData field, const QueryKnobValue& val) {
        b.appendBool(field, std::get<bool>(val));
    }
};

}  // namespace detail

/**
 * Typed descriptor. Self-registers into QueryKnobDescriptorSet at static init time.
 * fromBSON/toBSON auto-wired via ConverterTraits<T>.
 */
template <typename T>
struct QueryKnob : QueryKnobBase {
    QueryKnob(StringData name, ReadGlobalFn readFn) : QueryKnobBase(name, readFn) {
        fromBSON = &detail::ConverterTraits<T>::fromBSON;
        toBSON = &detail::ConverterTraits<T>::toBSON;
        QueryKnobDescriptorSet::get().add(*this);
    }
};

}  // namespace mongo
