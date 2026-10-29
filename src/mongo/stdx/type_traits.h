// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"
#include "mongo/util/modules.h"

#include <type_traits>

// TS2's `std::experimental::is_detected` and related traits.
// See https://en.cppreference.com/w/cpp/experimental/is_detected
//
// It's a utility for concisely writing traits. It allows a simple
// `Op<...>` alias template to be interrogated for substitution
// failure, and a few related utilities cover common uses of it.
//
// Examples:
//      template <typename T>
//      using HasAValueTypeOp = typename T::value_type;
//
//      template <typename T>
//      constexpr bool hasAValueType = stdx::is_detected_v<HasAValueType, T>;
//
//      // You can also access what the Op's type result was if it succeeds.
//      // If it fails, the typedef will be the sentinel `stdx::nonesuch` type.
//      template <typename T>
//      using InThatCaseWhatWasIt = stdx::is_detected_t<HasAValueType, T>;
//
//      // Or provide your own default:
//      template <typename T>
//      using ValueTypeOrVoid = stdx::is_detected_or_t<void, HasAValueType, T>;
//
// This std::experimental TS2 API may or may not be present in
// toolchain, so it's simplest to provide a full implementation
// here until such time as it's available in `std::` or the trait
// creation niche is filled by simpler language features (e.g.
// `concept`) and we all migrate off of direct SFINAE.
namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace stdx {
namespace detail {
template <typename Default, typename AlwaysVoid, template <class...> class Op, typename... Args>
struct Detector {
    using value_t = std::false_type;
    using type = Default;
};
template <typename Default, template <class...> class Op, typename... Args>
struct Detector<Default, std::void_t<Op<Args...>>, Op, Args...> {
    using value_t = std::true_type;
    using type = Op<Args...>;
};
}  // namespace detail

struct nonesuch {
    nonesuch() = delete;
    ~nonesuch() = delete;
    nonesuch(const nonesuch&) = delete;
    nonesuch& operator=(const nonesuch&) = delete;
};

template <template <class...> class Op, typename... Args>
using is_detected = typename detail::Detector<nonesuch, void, Op, Args...>::value_t;

template <template <class...> class Op, typename... Args>
using detected_t = typename detail::Detector<nonesuch, void, Op, Args...>::type;

template <template <class...> class Op, typename... Args>
constexpr bool is_detected_v = is_detected<Op, Args...>::value;

template <typename Default, template <class...> class Op, typename... Args>
using detected_or = detail::Detector<Default, void, Op, Args...>;

template <typename Default, template <class...> class Op, typename... Args>
using detected_or_t = typename detected_or<Default, Op, Args...>::type;

template <typename Expected, template <class...> class Op, typename... Args>
using is_detected_exact = std::is_same<Expected, detected_t<Op, Args...>>;

template <typename Expected, template <class...> class Op, typename... Args>
constexpr bool is_detected_exact_v = is_detected_exact<Expected, Op, Args...>::value;

template <typename To, template <class...> class Op, typename... Args>
using is_detected_convertible = std::is_convertible<detected_t<Op, Args...>, To>;

template <typename To, template <class...> class Op, typename... Args>
constexpr bool is_detected_convertible_v = is_detected_convertible<To, Op, Args...>::value;

}  // namespace stdx
}  // namespace mongo
