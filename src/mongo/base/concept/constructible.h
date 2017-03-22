#pragma once

#include <type_traits>

#include "mongo/stdx/type_traits.h"

namespace mongo {
namespace concept {

/**
 * The Constructable trait indicates whether `T` is constructible from `Constructible`.
 *
 * RETURNS: true if `T{ std::declval< Constructible >() }` is a valid expression and false
 * otherwise.
 */
template <typename T, typename Constructible, typename = void>
struct is_constructible : std::false_type {};

template <typename T, typename Constructible>
struct is_constructible<T,
                        Constructible,
                        stdx::void_t<decltype(T{std::declval<Constructible<T>>()})>>
    : std::true_type {};

/**
 * The Constructable concept models a type which can be passed to a single-argument constructor of
 * `T`.
 * This is not possible to describe in the type `Constructible`.
 *
 * The expression: `T{ std::declval< Constructible< T > >() }` should be valid.
 *
 * This concept is more broadly applicable than `ConvertibleTo`.  `ConvertibleTo` uses implicit
 * conversion, whereas `Constructible` uses direct construction.
 */
template <typename T>
struct Constructible {};
}  // namespace concept
}  // namespace mongo
