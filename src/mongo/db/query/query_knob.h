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
#include "mongo/db/server_parameter.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <concepts>
#include <type_traits>
#include <variant>
#include <vector>

#include <fmt/format.h>

namespace mongo {

// Removal sentinel (null on wire, used during PQS merge).
struct DeleteQueryKnobOverride {
    friend constexpr bool operator==(DeleteQueryKnobOverride, DeleteQueryKnobOverride) = default;
};

// Enum-typed knobs are stored as int.
using QueryKnobValue = std::variant<DeleteQueryKnobOverride, int, long long, double, bool>;

using ReadGlobalFn = QueryKnobValue (*)(StringData);

template <typename S>
concept AtomicLoadable = requires(const S& s) {
    { s.load() };
};

template <typename S>
concept EnumServerParameter = std::derived_from<S, ServerParameter> && requires(S s) {
    { s._data.get() };
};

/**
 * Concept-dispatched reader. Atomic globals are handled by the 'storage' NTTP which produces one
 * instantiation per global variable.
 */
template <auto& storage>
requires AtomicLoadable<std::remove_cvref_t<decltype(storage)>>
QueryKnobValue readGlobalValue(StringData paramName) {
    return QueryKnobValue{storage.load()};
}

/**
 * Enum knobs are handled by looking up the ServerParameter by its name and type. Enum values are
 * internally represented as ints.
 */
template <typename SPT>
requires EnumServerParameter<SPT>
QueryKnobValue readGlobalValue(StringData paramName) {
    try {
        auto* sp = ServerParameterSet::getNodeParameterSet()->template get<SPT>(paramName);
        return QueryKnobValue{static_cast<int>(sp->_data.get())};
    } catch (const DBException&) {
        tasserted(exceptionToStatus().withContext(
            fmt::format("Failed to read query knob global '{}'", paramName)));
    }
}

/**
 * Type-erased descriptor for a single query knob. Each typed QueryKnob<T> inherits from this and
 * self-registers into QueryKnobDescriptorSet at static init time. The index field is left as a
 * sentinel (~0) until QueryKnobRegistry assigns dense indices during startup.
 *
 * Non-copyable to prevent object slicing when used through the base pointer.
 */
class QueryKnobBase {
public:
    QueryKnobBase(StringData paramName, ReadGlobalFn readFn)
        : paramName(paramName), _readFn(readFn) {}
    virtual ~QueryKnobBase() = default;

    QueryKnobBase(const QueryKnobBase&) = delete;
    QueryKnobBase(QueryKnobBase&&) = delete;
    QueryKnobBase& operator=(const QueryKnobBase&) = delete;
    QueryKnobBase& operator=(QueryKnobBase&&) = delete;

    virtual QueryKnobValue fromBSON(const BSONElement& elem) = 0;
    virtual void toBSON(BSONObjBuilder& b, StringData field, const QueryKnobValue& val) = 0;

    QueryKnobValue readGlobal() const {
        return _readFn(paramName);
    }

    // TODO SERVER-125549: refactor to use a strongly typed KnobId.
    size_t index = ~size_t{0};  // Sentinel until QueryKnobRegistry assigns a dense index.
    StringData paramName;

private:
    ReadGlobalFn _readFn;
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
class QueryKnob final : public QueryKnobBase {
public:
    QueryKnob(StringData name, ReadGlobalFn readFn) : QueryKnobBase(name, readFn) {
        QueryKnobDescriptorSet::get().add(*this);
    }

    QueryKnobValue fromBSON(const BSONElement& elem) override {
        return detail::ConverterTraits<T>::fromBSON(elem);
    }

    void toBSON(BSONObjBuilder& b, StringData field, const QueryKnobValue& val) override {
        return detail::ConverterTraits<T>::toBSON(b, field, val);
    }
};

}  // namespace mongo
