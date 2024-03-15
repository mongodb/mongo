/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_FunctionTypeTraits_h
#define mozilla_FunctionTypeTraits_h

#include <cstddef> /* for size_t */
#include <tuple>

namespace mozilla {

// Main FunctionTypeTraits declaration, taking one template argument.
//
// Given a function type, FunctionTypeTraits will expose the following members:
// - ReturnType: Return type.
// - arity: Number of parameters (size_t).
// - ParameterType<N>: Type of the Nth** parameter, 0-indexed.
//
// ** `ParameterType<N>` with `N` >= `arity` is allowed and gives `void`.
// This prevents compilation errors when trying to access a type outside of the
// function's parameters, which is useful for parameters checks, e.g.:
//   template<typename F>
//   auto foo(F&&)
//    -> enable_if(FunctionTypeTraits<F>::arity == 1 &&
//                 is_same<FunctionTypeTraits<F>::template ParameterType<0>,
//                         int>::value,
//                 void)
//   {
//     // This function will only be enabled if `F` takes one `int`.
//     // Without the permissive ParameterType<any N>, it wouldn't even compile.
//
// Note: FunctionTypeTraits does not work with generic lambdas `[](auto&) {}`,
// because parameter types cannot be known until an actual invocation when types
// are inferred from the given arguments.
template <typename T>
struct FunctionTypeTraits;

// Remove reference and pointer wrappers, if any.
template <typename T>
struct FunctionTypeTraits<T&> : public FunctionTypeTraits<T> {};
template <typename T>
struct FunctionTypeTraits<T&&> : public FunctionTypeTraits<T> {};
template <typename T>
struct FunctionTypeTraits<T*> : public FunctionTypeTraits<T> {};

// Extract `operator()` function from callables (e.g. lambdas, std::function).
template <typename T>
struct FunctionTypeTraits
    : public FunctionTypeTraits<decltype(&T::operator())> {};

namespace detail {

// If `safe`, retrieve the `N`th type from `As`, otherwise `void`.
// See top description for reason.
template <bool safe, size_t N, typename... As>
struct TupleElementSafe;
template <size_t N, typename... As>
struct TupleElementSafe<true, N, As...> {
  using Type = typename std::tuple_element<N, std::tuple<As...>>::type;
};
template <size_t N, typename... As>
struct TupleElementSafe<false, N, As...> {
  using Type = void;
};

template <typename R, typename... As>
struct FunctionTypeTraitsHelper {
  using ReturnType = R;
  static constexpr size_t arity = sizeof...(As);
  template <size_t N>
  using ParameterType =
      typename TupleElementSafe<(N < sizeof...(As)), N, As...>::Type;
};

}  // namespace detail

// Specialization for free functions.
template <typename R, typename... As>
struct FunctionTypeTraits<R(As...)>
    : detail::FunctionTypeTraitsHelper<R, As...> {};

// Specialization for non-const member functions.
template <typename C, typename R, typename... As>
struct FunctionTypeTraits<R (C::*)(As...)>
    : detail::FunctionTypeTraitsHelper<R, As...> {};

// Specialization for const member functions.
template <typename C, typename R, typename... As>
struct FunctionTypeTraits<R (C::*)(As...) const>
    : detail::FunctionTypeTraitsHelper<R, As...> {};

#ifdef NS_HAVE_STDCALL
// Specialization for __stdcall free functions.
template <typename R, typename... As>
struct FunctionTypeTraits<R NS_STDCALL(As...)>
    : detail::FunctionTypeTraitsHelper<R, As...> {};

// Specialization for __stdcall non-const member functions.
template <typename C, typename R, typename... As>
struct FunctionTypeTraits<R (NS_STDCALL C::*)(As...)>
    : detail::FunctionTypeTraitsHelper<R, As...> {};

// Specialization for __stdcall const member functions.
template <typename C, typename R, typename... As>
struct FunctionTypeTraits<R (NS_STDCALL C::*)(As...) const>
    : detail::FunctionTypeTraitsHelper<R, As...> {};
#endif  // NS_HAVE_STDCALL

}  // namespace mozilla

#endif  // mozilla_FunctionTypeTraits_h
