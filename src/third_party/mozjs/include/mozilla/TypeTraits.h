/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Template-based metaprogramming and type-testing facilities. */

#ifndef mozilla_TypeTraits_h
#define mozilla_TypeTraits_h

#include "mozilla/Types.h"

#include <type_traits>
#include <utility>

/*
 * These traits are approximate copies of the traits and semantics from C++11's
 * <type_traits> header.  Don't add traits not in that header!  When all
 * platforms provide that header, we can convert all users and remove this one.
 */

namespace mozilla {

/* 20.9.4 Unary type traits [meta.unary] */

/* 20.9.4.3 Type properties [meta.unary.prop] */

/**
 * Traits class for identifying POD types.  Until C++11 there's no automatic
 * way to detect PODs, so for the moment this is done manually.  Users may
 * define specializations of this class that inherit from std::true_type and
 * std::false_type (or equivalently std::integral_constant<bool, true or
 * false>, or conveniently from mozilla::IsPod for composite types) as needed to
 * ensure correct IsPod behavior.
 */
template <typename T>
struct IsPod : public std::false_type {};

template <>
struct IsPod<char> : std::true_type {};
template <>
struct IsPod<signed char> : std::true_type {};
template <>
struct IsPod<unsigned char> : std::true_type {};
template <>
struct IsPod<short> : std::true_type {};
template <>
struct IsPod<unsigned short> : std::true_type {};
template <>
struct IsPod<int> : std::true_type {};
template <>
struct IsPod<unsigned int> : std::true_type {};
template <>
struct IsPod<long> : std::true_type {};
template <>
struct IsPod<unsigned long> : std::true_type {};
template <>
struct IsPod<long long> : std::true_type {};
template <>
struct IsPod<unsigned long long> : std::true_type {};
template <>
struct IsPod<bool> : std::true_type {};
template <>
struct IsPod<float> : std::true_type {};
template <>
struct IsPod<double> : std::true_type {};
template <>
struct IsPod<wchar_t> : std::true_type {};
template <>
struct IsPod<char16_t> : std::true_type {};
template <typename T>
struct IsPod<T*> : std::true_type {};

namespace detail {

struct DoIsDestructibleImpl {
  template <typename T, typename = decltype(std::declval<T&>().~T())>
  static std::true_type test(int);
  template <typename T>
  static std::false_type test(...);
};

template <typename T>
struct IsDestructibleImpl : public DoIsDestructibleImpl {
  typedef decltype(test<T>(0)) Type;
};

}  // namespace detail

/**
 * IsDestructible determines whether a type has a public destructor.
 *
 * struct S0 {};                    // Implicit default destructor.
 * struct S1 { ~S1(); };
 * class C2 { ~C2(); };             // private destructor.
 *
 * mozilla::IsDestructible<S0>::value is true;
 * mozilla::IsDestructible<S1>::value is true;
 * mozilla::IsDestructible<C2>::value is false.
 */
template <typename T>
struct IsDestructible : public detail::IsDestructibleImpl<T>::Type {};

} /* namespace mozilla */

#endif /* mozilla_TypeTraits_h */
