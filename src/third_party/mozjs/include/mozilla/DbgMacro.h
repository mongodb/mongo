/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_DbgMacro_h
#define mozilla_DbgMacro_h

/* a MOZ_DBG macro that outputs a wrapped value to stderr then returns it */

#include "mozilla/MacroForEach.h"
#include "mozilla/Span.h"

#include <stdio.h>
#include <sstream>

template <typename T>
class nsTSubstring;

#ifdef ANDROID
#  include <android/log.h>
#endif

namespace mozilla {

namespace detail {

// Predicate to check whether T can be inserted into an ostream.
template <typename T, typename = decltype(std::declval<std::ostream&>()
                                          << std::declval<T>())>
std::true_type supports_os_test(const T&);
std::false_type supports_os_test(...);

template <typename T>
using supports_os = decltype(supports_os_test(std::declval<T>()));

}  // namespace detail

// Helper function to write a value to an ostream.
//
// This handles pointer values where the type being pointed to supports being
// inserted into an ostream, and we write out the value being pointed to in
// addition to the pointer value.
template <typename T>
auto DebugValue(std::ostream& aOut, T* aValue)
    -> std::enable_if_t<mozilla::detail::supports_os<T>::value, std::ostream&> {
  if (aValue) {
    aOut << *aValue << " @ " << aValue;
  } else {
    aOut << "null";
  }
  return aOut;
}

// Helper function to write a value to an ostream.
//
// This handles all pointer types that cannot be dereferenced and inserted into
// an ostream.
template <typename T>
auto DebugValue(std::ostream& aOut, T* aValue)
    -> std::enable_if_t<!mozilla::detail::supports_os<T>::value,
                        std::ostream&> {
  return aOut << aValue;
}

// Helper function to write a value to an ostream.
//
// This handles XPCOM string types.
template <typename T>
auto DebugValue(std::ostream& aOut, const T& aValue)
    -> std::enable_if_t<std::is_base_of<nsTSubstring<char>, T>::value ||
                            std::is_base_of<nsTSubstring<char16_t>, T>::value,
                        std::ostream&> {
  return aOut << '"' << aValue << '"';
}

// Helper function to write a value to an ostream.
//
// This handles all other types.
template <typename T>
auto DebugValue(std::ostream& aOut, const T& aValue)
    -> std::enable_if_t<!std::is_base_of<nsTSubstring<char>, T>::value &&
                            !std::is_base_of<nsTSubstring<char16_t>, T>::value,
                        std::ostream&> {
  return aOut << aValue;
}

namespace detail {

// Helper function template for MOZ_DBG.
template <typename T>
auto&& MozDbg(const char* aFile, int aLine, const char* aExpression,
              T&& aValue) {
  std::ostringstream s;
  s << "[MozDbg] [" << aFile << ':' << aLine << "] " << aExpression << " = ";
  mozilla::DebugValue(s, std::forward<T>(aValue));
  s << '\n';
#ifdef ANDROID
  __android_log_print(ANDROID_LOG_INFO, "Gecko", "%s", s.str().c_str());
#else
  fputs(s.str().c_str(), stderr);
#endif
  return std::forward<T>(aValue);
}

}  // namespace detail

}  // namespace mozilla

template <class ElementType, size_t Extent>
std::ostream& operator<<(std::ostream& aOut,
                         const mozilla::Span<ElementType, Extent>& aSpan) {
  aOut << '[';
  if (!aSpan.IsEmpty()) {
    aOut << aSpan[0];
    for (size_t i = 1; i < aSpan.Length(); ++i) {
      aOut << ", " << aSpan[i];
    }
  }
  return aOut << ']';
}

// Don't define this for char[], since operator<<(ostream&, char*) is already
// defined.
template <typename T, size_t N,
          typename = std::enable_if_t<!std::is_same<T, char>::value>>
std::ostream& operator<<(std::ostream& aOut, const T (&aArray)[N]) {
  return aOut << mozilla::Span(aArray);
}

// MOZ_DBG is a macro like the Rust dbg!() macro -- it will print out the
// expression passed to it to stderr and then return the value.  It is not
// available in MOZILLA_OFFICIAL builds, so you shouldn't land any uses of it in
// the tree.
//
// It should work for any type T that has an operator<<(std::ostream&, const T&)
// defined for it.
//
// Note 1: Using MOZ_DBG may cause copies to be made of temporary values:
//
//   struct A {
//     A(int);
//     A(const A&);
//
//     int x;
//   };
//
//   void f(A);
//
//   f(A{1});  // may (and, in C++17, will) elide the creation of a temporary
//             // for A{1} and instead initialize the function argument
//             // directly using the A(int) constructor
//
//   f(MOZ_DBG(A{1}));  // will create and return a temporary for A{1}, which
//                      // then will be passed to the A(const A&) copy
//                      // constructor to initialize f's argument
//
// Note 2: MOZ_DBG cannot be used to wrap a prvalue that is being used to
// initialize an object if its type has no move constructor:
//
//   struct B {
//     B() = default;
//     B(B&&) = delete;
//   };
//
//   B b1 = B();  // fine, initializes b1 directly
//
//   B b2 = MOZ_DBG(B());  // compile error: MOZ_DBG needs to materialize a
//                         // temporary for B() so it can be passed to
//                         // operator<<, but that temporary is returned from
//                         // MOZ_DBG as an rvalue reference and so wants to
//                         // invoke B's move constructor to initialize b2
#ifndef MOZILLA_OFFICIAL
#  define MOZ_DBG(...) \
    mozilla::detail::MozDbg(__FILE__, __LINE__, #__VA_ARGS__, __VA_ARGS__)
#endif

// Helper macro for MOZ_DEFINE_DBG.
#define MOZ_DBG_FIELD(name_) << #name_ << " = " << aValue.name_

// Macro to define an operator<<(ostream&) for a struct or class that displays
// the type name and the values of the specified member variables.  Must be
// called inside the struct or class.
//
// For example:
//
//   struct Point {
//     float x;
//     float y;
//
//     MOZ_DEFINE_DBG(Point, x, y)
//   };
//
// generates an operator<< that outputs strings like
// "Point { x = 1.0, y = 2.0 }".
#define MOZ_DEFINE_DBG(type_, ...)                                           \
  friend std::ostream& operator<<(std::ostream& aOut, const type_& aValue) { \
    return aOut << #type_                                                    \
                << (MOZ_ARG_COUNT(__VA_ARGS__) == 0 ? "" : " { ")            \
                       MOZ_FOR_EACH_SEPARATED(MOZ_DBG_FIELD, (<< ", "), (),  \
                                              (__VA_ARGS__))                 \
                << (MOZ_ARG_COUNT(__VA_ARGS__) == 0 ? "" : " }");            \
  }

#endif  // mozilla_DbgMacro_h
