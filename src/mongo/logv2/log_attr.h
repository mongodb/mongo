// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <tuple>
#include <type_traits>

#include <fmt/format.h>

namespace mongo {
namespace logv2 {
namespace detail {
/**
 * Helper to be used inside logAttrs() functions, captures rvalues by value so they don't go out of
 * scope Can create a tuple of loggable named attributes for the logger
 */
template <typename... Ts>
class ComposedAttr {
public:
    ComposedAttr(Ts&&... args) : _values(std::forward<Ts>(args)...) {}

    /**
     * Creates a flattened tuple of loggable named attributes
     */
    auto attributes() const;

private:
    std::tuple<Ts...> _values;
};

template <typename T>
struct [[MONGO_MOD_PUBLIC]] NamedArg {
    const char* name;
    const T& value;
};

template <typename T>
struct isNamedArg : public std::false_type {};

template <typename T>
struct isNamedArg<NamedArg<T>> : public std::true_type {};

template <typename T>
concept IsNamedArg = isNamedArg<T>::value;

struct [[MONGO_MOD_NEEDS_REPLACEMENT]] AttrUdl {
    const char* name;

    template <typename T>
    NamedArg<T> operator=(T&& v) const {
        return NamedArg<T>{name, std::forward<T>(v)};
    }
};

/**
 * Helper to make regular attributes composable with combine()
 */
template <typename T>
auto logAttrs(const NamedArg<T>& a) {
    return a;
}

/**
 * Flattens the input into a single tuple (no tuples of tuples). Passes through the input by
 * reference by possible. May only be used at the call side of the log system to avoid dangling
 * references.
 */
template <typename T>
std::tuple<const T&> toFlatAttributesTupleRef(const T& arg) {
    return {arg};
}

template <typename... Ts>
auto toFlatAttributesTupleRef(const ComposedAttr<Ts...>& arg) {
    return arg.attributes();
}

/**
 * Same as above but does not pass by reference. Needs to be used when building composed hierarchies
 * in helper functions
 */
template <typename T>
std::tuple<T> toFlatAttributesTuple(const T& arg) {
    return {arg};
}
template <typename... Ts>
auto toFlatAttributesTuple(const ComposedAttr<Ts...>& arg) {
    return arg.attributes();
}

template <typename... Ts>
auto ComposedAttr<Ts...>::attributes() const {
    // logAttrs() converts the input to a loggable named attribute, user implementation
    return std::apply(
        [this](auto&&... args) { return std::tuple_cat(toFlatAttributesTuple(logAttrs(args))...); },
        _values);
}

}  // namespace detail

/**
 * Combines multiple attributes to be returned in user defined logAttrs() functions
 */
template <typename... Ts>
[[MONGO_MOD_PUBLIC]] auto multipleAttrs(Ts&&... attrs) {
    // We can capture lvalue references as reference otherwise this is a temporary object that needs
    // to be stored during the lifetime of the log statement.
    return detail::ComposedAttr<
        std::conditional_t<std::is_lvalue_reference_v<Ts>, Ts, std::remove_reference_t<Ts>>...>(
        std::forward<Ts>(attrs)...);
}

}  // namespace logv2

inline namespace literals {
[[MONGO_MOD_PUBLIC]] constexpr logv2::detail::AttrUdl operator""_attr(const char* name,
                                                                      std::size_t) {
    return {name};
}
}  // namespace literals
}  // namespace mongo
