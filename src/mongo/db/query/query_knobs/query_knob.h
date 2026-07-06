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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace mongo {

// Removal sentinel (null on wire, used during PQS merge).
struct DeleteQueryKnobOverride {
    friend constexpr auto operator<=>(DeleteQueryKnobOverride, DeleteQueryKnobOverride) = default;
};

// Enum-typed knobs are stored as int.
using QueryKnobValue = std::variant<DeleteQueryKnobOverride, int, long long, double, bool>;

using ReadGlobalFn = QueryKnobValue (*)(std::string_view);

template <typename S>
concept AtomicLoadable = requires(const S& s) {
    { s.load() };
};

template <typename S>
concept EnumServerParameter = std::derived_from<S, ServerParameter> && requires(S s) {
    { s._data.get() };
};

/**
 * Strong id type to identify query knobs. Defaults to 'kUninitialized'.
 */
struct QueryKnobId {
    using value_t = std::uint16_t;
    friend auto operator<=>(const QueryKnobId& lhs, const QueryKnobId& rhs) = default;
    value_t value;
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
    static void toBSON(BSONObjBuilder& b, std::string_view field, const QueryKnobValue& val) {
        b.append(field, idl::serialize(static_cast<T>(std::get<int>(val))));
    }
    static void appendType(BSONObjBuilder* b) {
        b->append("type", "enum");
        BSONArrayBuilder arr(b->subarrayStart("allowedValues"));
        for (size_t i = 0; i < idlEnumCount<T>; ++i) {
            arr.append(idl::serialize(static_cast<T>(i)));
        }
    }
};

template <>
struct ConverterTraits<int> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        // Coerce across BSON numeric types exactly as setParameter does, including truncating
        // doubles toward zero.
        int out;
        uassertStatusOK(elem.tryCoerce(&out));
        return out;
    }
    static void toBSON(BSONObjBuilder& b, std::string_view field, const QueryKnobValue& val) {
        b.append(field, std::get<int>(val));
    }
    static void appendType(BSONObjBuilder* b) {
        b->append("type", "int");
    }
};

template <>
struct ConverterTraits<long long> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        // Coerce across BSON numeric types exactly as setParameter does, including truncating
        // doubles toward zero.
        long long out;
        uassertStatusOK(elem.tryCoerce(&out));
        return out;
    }
    static void toBSON(BSONObjBuilder& b, std::string_view field, const QueryKnobValue& val) {
        b.append(field, std::get<long long>(val));
    }
    static void appendType(BSONObjBuilder* b) {
        b->append("type", "long long");
    }
};

template <>
struct ConverterTraits<double> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        // Coerce across BSON numeric types, matching setParameter semantics.
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "query knob double value must be numeric, got "
                              << typeName(elem.type()),
                elem.isNumber());
        const double val = elem.numberDouble();
        uassert(ErrorCodes::BadValue, "query knob double value must not be NaN", !std::isnan(val));
        return val;
    }
    static void toBSON(BSONObjBuilder& b, std::string_view field, const QueryKnobValue& val) {
        b.append(field, std::get<double>(val));
    }
    static void appendType(BSONObjBuilder* b) {
        b->append("type", "double");
    }
};

template <>
struct ConverterTraits<bool> {
    static QueryKnobValue fromBSON(const BSONElement& elem) {
        return elem.Bool();
    }
    static void toBSON(BSONObjBuilder& b, std::string_view field, const QueryKnobValue& val) {
        b.appendBool(field, std::get<bool>(val));
    }
    static void appendType(BSONObjBuilder* b) {
        b->append("type", "bool");
    }
};

/**
 * Helpers for inferring the query knob value type from a global.
 */
template <auto& V>
requires AtomicLoadable<std::remove_cvref_t<decltype(V)>>
auto queryKnobValueType() -> decltype(V.load());

template <typename SPT>
requires EnumServerParameter<SPT>
auto queryKnobValueType() -> std::remove_cvref_t<decltype(std::declval<SPT>()._data.get())>;

}  // namespace detail

/**
 * Typed handle for a query knob. T is a phantom type used to give callers like
 * `QueryKnobConfiguration::get<T>(const QueryKnob<T>&)` compile-time type safety.
 */
template <typename T>
struct QueryKnob {
    QueryKnobId id;
};

}  // namespace mongo

// clang-format off
/**
 * X-macro framework for declaring and registering groups of QueryKnobs.
 *
 * Define an EXPAND table with one row per knob:
 *
 *   #define MY_EXPAND(KNOB)                                              \
 *       KNOB(myIntKnob,  "myIntParam",  gMyIntAtomicGlobal, getMyInt)    \
 *       KNOB(myEnumKnob, "myEnumParam", MyEnumServerParam,  getMyEnum)
 *
 *   var    — C++ identifier for the QueryKnob variable
 *   name   — server parameter string (must match the ServerParameter IDL name)
 *   global — atomic global for scalar knobs; ServerParameter type for enum knobs
 *   getter — name of the accessor method generated on the AccessorMixin for this knob
 *
 * In the group header (inside the knob namespace):
 *   DECLARE_QUERY_KNOBS(GroupName, MY_EXPAND)
 *
 * In the group .cpp (inside the knob namespace):
 *   REGISTER_QUERY_KNOBS(GroupName, MY_EXPAND)  // see query_knob_registry.h
 *
 * See query_knob_test_knobs.h/cpp for a worked example.
 */

// Internal: emits one extern QueryKnob declaration per EXPAND row. The trailing getter
// column is unused here.
#define MONGO_DETAIL_DECLARE_QUERY_KNOB(var, name, global, ...) \
    extern const QueryKnob<decltype(detail::queryKnobValueType<global>())> var;

#define MONGO_DETAIL_DEFINE_GETTER(var, name, global, getter) \
    auto getter() const {                                     \
        return static_cast<const Derived*>(this)->get(var);   \
    }

#define MONGO_DETAIL_DEFINE_ACCESSOR_MIXIN(group, EXPAND)   \
    template <typename Derived>                             \
    class AccessorMixin##group {                            \
    public:                                                 \
        EXPAND(MONGO_DETAIL_DEFINE_GETTER)                  \
    };

// Declares all knobs in EXPAND. Place in the group header inside the knob namespace.
#define DECLARE_QUERY_KNOBS(group, EXPAND)              \
    EXPAND(MONGO_DETAIL_DECLARE_QUERY_KNOB)             \
    MONGO_DETAIL_DEFINE_ACCESSOR_MIXIN(group, EXPAND)
// clang-format on
