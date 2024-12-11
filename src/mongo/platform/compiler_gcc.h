/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
 * Compiler-specific implementations for gcc.
 *
 * Refer to mongo/platform/compiler.h for usage documentation.
 */

#pragma once


#ifdef __clang__
// Annotating methods with [[lifetimebound]] allows the compiler to do some more lifetime checking
// (e.g., "returned value should not outlive *this") and emit warnings. See
// https://clang.llvm.org/docs/AttributeReference.html#lifetimebound
#if defined(__has_cpp_attribute) && __has_cpp_attribute(clang::lifetimebound)
#define MONGO_COMPILER_LIFETIME_BOUND [[clang::lifetimebound]]
#else
#define MONGO_COMPILER_LIFETIME_BOUND
#endif

#else

#if defined(__has_cpp_attribute) && __has_cpp_attribute(lifetimebound)
#define MONGO_COMPILER_LIFETIME_BOUND [[lifetimebound]]
#else
#define MONGO_COMPILER_LIFETIME_BOUND
#endif

#endif

#define MONGO_COMPILER_COLD_FUNCTION __attribute__((__cold__))
#define MONGO_COMPILER_NORETURN __attribute__((__noreturn__, __cold__))

#define MONGO_WARN_UNUSED_RESULT_CLASS [[nodiscard]]
#define MONGO_WARN_UNUSED_RESULT_FUNCTION [[nodiscard]]


#define MONGO_COMPILER_ALIGN_TYPE(ALIGNMENT) __attribute__((__aligned__(ALIGNMENT)))

#define MONGO_COMPILER_ALIGN_VARIABLE(ALIGNMENT) __attribute__((__aligned__(ALIGNMENT)))

// NOTE(schwerin): These visibility and calling-convention macro definitions assume we're not using
// GCC/CLANG to target native Windows. If/when we decide to do such targeting, we'll need to change
// compiler flags on Windows to make sure we use an appropriate calling convention, and configure
// MONGO_COMPILER_API_EXPORT, MONGO_COMPILER_API_IMPORT and MONGO_COMPILER_API_CALLING_CONVENTION
// correctly.  I believe "correctly" is the following:
//
// #ifdef _WIN32
// #define MONGO_COMIPLER_API_EXPORT __attribute__(( __dllexport__ ))
// #define MONGO_COMPILER_API_IMPORT __attribute__(( __dllimport__ ))
// #ifdef _M_IX86
// #define MONGO_COMPILER_API_CALLING_CONVENTION __attribute__((__cdecl__))
// #else
// #define MONGO_COMPILER_API_CALLING_CONVENTION
// #endif
// #else ... fall through to the definitions below.

#define MONGO_COMPILER_API_EXPORT __attribute__((__visibility__("default")))
#define MONGO_COMPILER_API_IMPORT
#define MONGO_COMPILER_API_HIDDEN_FUNCTION __attribute__((visibility("hidden")))
#define MONGO_COMPILER_API_CALLING_CONVENTION

#define MONGO_likely(x) static_cast<bool>(__builtin_expect(static_cast<bool>(x), 1))
#define MONGO_unlikely(x) static_cast<bool>(__builtin_expect(static_cast<bool>(x), 0))

#define MONGO_COMPILER_ALWAYS_INLINE [[gnu::always_inline]]

#define MONGO_COMPILER_UNREACHABLE __builtin_unreachable()

#define MONGO_COMPILER_NOINLINE [[gnu::noinline]]

#define MONGO_COMPILER_RETURNS_NONNULL [[gnu::returns_nonnull]]

#define MONGO_COMPILER_MALLOC [[gnu::malloc]]

#define MONGO_COMPILER_ALLOC_SIZE(varindex) [[gnu::alloc_size(varindex)]]

#define MONGO_COMPILER_NO_UNIQUE_ADDRESS [[no_unique_address]]

#define MONGO_COMPILER_USED [[gnu::used]]

#if defined(__clang__)
#define MONGO_GSL_POINTER [[gsl::Pointer]]
#else
#define MONGO_GSL_POINTER
#endif

// Both GCC and clang can use this abstract macro, mostly as an implementation detail. It's up to
// callers to know when GCC and clang disagree on the name of the warning.
#if defined(__clang__)
#define MONGO_COMPILER_DIAGNOSTIC_PUSH MONGO_COMPILER_PRAGMA(clang diagnostic push)
#define MONGO_COMPILER_DIAGNOSTIC_POP MONGO_COMPILER_PRAGMA(clang diagnostic pop)
#define MONGO_COMPILER_DIAGNOSTIC_IGNORED(w) MONGO_COMPILER_PRAGMA(clang diagnostic ignored w)
#else
#define MONGO_COMPILER_DIAGNOSTIC_PUSH MONGO_COMPILER_PRAGMA(GCC diagnostic push)
#define MONGO_COMPILER_DIAGNOSTIC_POP MONGO_COMPILER_PRAGMA(GCC diagnostic pop)
#define MONGO_COMPILER_DIAGNOSTIC_IGNORED(w) MONGO_COMPILER_PRAGMA(GCC diagnostic ignored w)
#endif  // clang

#if !defined(__clang__) && __GNUC__ >= 14

// TODO(SERVER-97447): We ignore these warnings on GCC 14 to facilitate transition to the v5
// toolchain. They should be investigated more deeply by the teams owning each callsite.
#define MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL(w) MONGO_COMPILER_DIAGNOSTIC_IGNORED(w)

// We selectively ignore -Wstringop-overread on GCC 14 due to a known bug affecting
// boost::container::small_vector: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=108197.
#define MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_BOOST_SMALL_VECTOR \
    MONGO_COMPILER_DIAGNOSTIC_IGNORED("-Wstringop-overread")

// We selectively ignore -Wstringop-overflow on GCC 14 due to strong suspicion that they are
// false-positives. They involve an atomic read overflowing the destination, likely due to the
// compiler incorrectly believing they might be referencing a NULL pointer.
#define MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_READ \
    MONGO_COMPILER_DIAGNOSTIC_IGNORED("-Wstringop-overflow")

#else
#define MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL(w)
#define MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_BOOST_SMALL_VECTOR
#define MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_READ
#endif
