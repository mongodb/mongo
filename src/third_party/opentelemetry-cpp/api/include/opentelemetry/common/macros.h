// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/*
   OPENTELEMETRY_HAVE_BUILTIN&OPENTELEMETRY_HAVE_FEATURE

   Checks whether the compiler supports a Clang Feature Checking Macro, and if
   so, checks whether it supports the provided builtin function "x" where x
   is one of the functions noted in
   https://clang.llvm.org/docs/LanguageExtensions.html

   Note: Use this macro to avoid an extra level of #ifdef __has_builtin check.
   http://releases.llvm.org/3.3/tools/clang/docs/LanguageExtensions.html
*/
#if !defined(OPENTELEMETRY_HAVE_BUILTIN)
#  ifdef __has_builtin
#    define OPENTELEMETRY_HAVE_BUILTIN(x) __has_builtin(x)
#  else
#    define OPENTELEMETRY_HAVE_BUILTIN(x) 0
#  endif
#endif

#if !defined(OPENTELEMETRY_HAVE_FEATURE)
#  ifdef __has_feature
#    define OPENTELEMETRY_HAVE_FEATURE(f) __has_feature(f)
#  else
#    define OPENTELEMETRY_HAVE_FEATURE(f) 0
#  endif
#endif

/*
   has feature

   OPENTELEMETRY_HAVE_ATTRIBUTE

   A function-like feature checking macro that is a wrapper around
   `__has_attribute`, which is defined by GCC 5+ and Clang and evaluates to a
   nonzero constant integer if the attribute is supported or 0 if not.

   It evaluates to zero if `__has_attribute` is not defined by the compiler.

   GCC: https://gcc.gnu.org/gcc-5/changes.html
   Clang: https://clang.llvm.org/docs/LanguageExtensions.html
*/
#if !defined(OPENTELEMETRY_HAVE_ATTRIBUTE)
#  ifdef __has_attribute
#    define OPENTELEMETRY_HAVE_ATTRIBUTE(x) __has_attribute(x)
#  else
#    define OPENTELEMETRY_HAVE_ATTRIBUTE(x) 0
#  endif
#endif

/*
   OPENTELEMETRY_HAVE_CPP_ATTRIBUTE

   A function-like feature checking macro that accepts C++11 style attributes.
   It's a wrapper around `__has_cpp_attribute`, defined by ISO C++ SD-6
   (https://en.cppreference.com/w/cpp/experimental/feature_test). If we don't
   find `__has_cpp_attribute`, will evaluate to 0.
*/
#if !defined(OPENTELEMETRY_HAVE_CPP_ATTRIBUTE)
#  if defined(__cplusplus) && defined(__has_cpp_attribute)
// NOTE: requiring __cplusplus above should not be necessary, but
// works around https://bugs.llvm.org/show_bug.cgi?id=23435.
#    define OPENTELEMETRY_HAVE_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#  else
#    define OPENTELEMETRY_HAVE_CPP_ATTRIBUTE(x) 0
#  endif
#endif

/*
   Expected usage pattern:

   if OPENTELEMETRY_LIKELY_CONDITION (ptr != nullptr)
   {
     do_something_likely();
   } else {
     do_something_unlikely();
   }

   This pattern works with gcc/clang and __builtin_expect(),
   as well as with C++20.
   It is unclear if __builtin_expect() will be deprecated
   in favor of C++20 [[likely]] or not.

   OPENTELEMETRY_LIKELY_CONDITION is preferred over OPENTELEMETRY_LIKELY,
   to be revisited when C++20 is required.
*/

#if !defined(OPENTELEMETRY_LIKELY_CONDITION) && defined(__cplusplus)
// Only use likely with C++20
#  if __cplusplus >= 202002L
// GCC 9 has likely attribute but do not support declare it at the beginning of statement
#    if defined(__has_cpp_attribute) && (defined(__clang__) || !defined(__GNUC__) || __GNUC__ > 9)
#      if __has_cpp_attribute(likely)
#        define OPENTELEMETRY_LIKELY_CONDITION(C) (C) [[likely]]
#      endif
#    endif
#  endif
#endif
#if !defined(OPENTELEMETRY_LIKELY_CONDITION) && (defined(__clang__) || defined(__GNUC__))
// Only use if supported by the compiler
#  define OPENTELEMETRY_LIKELY_CONDITION(C) (__builtin_expect(!!(C), true))
#endif
#ifndef OPENTELEMETRY_LIKELY_CONDITION
// Do not use likely annotations
#  define OPENTELEMETRY_LIKELY_CONDITION(C) (C)
#endif

#if !defined(OPENTELEMETRY_UNLIKELY_CONDITION) && defined(__cplusplus)
// Only use unlikely with C++20
#  if __cplusplus >= 202002L
// GCC 9 has unlikely attribute but do not support declare it at the beginning of statement
#    if defined(__has_cpp_attribute) && (defined(__clang__) || !defined(__GNUC__) || __GNUC__ > 9)
#      if __has_cpp_attribute(unlikely)
#        define OPENTELEMETRY_UNLIKELY_CONDITION(C) (C) [[unlikely]]
#      endif
#    endif
#  endif
#endif
#if !defined(OPENTELEMETRY_UNLIKELY_CONDITION) && (defined(__clang__) || defined(__GNUC__))
// Only use if supported by the compiler
#  define OPENTELEMETRY_UNLIKELY_CONDITION(C) (__builtin_expect(!!(C), false))
#endif
#ifndef OPENTELEMETRY_UNLIKELY_CONDITION
// Do not use unlikely annotations
#  define OPENTELEMETRY_UNLIKELY_CONDITION(C) (C)
#endif

/*
   Expected usage pattern:

   if (ptr != nullptr)
   OPENTELEMETRY_LIKELY
   {
     do_something_likely();
   } else {
     do_something_unlikely();
   }

   This pattern works starting with C++20.
   See https://en.cppreference.com/w/cpp/language/attributes/likely

   Please use OPENTELEMETRY_LIKELY_CONDITION instead for now.
*/

#if !defined(OPENTELEMETRY_LIKELY) && defined(__cplusplus)
// Only use likely with C++20
#  if __cplusplus >= 202002L
// GCC 9 has likely attribute but do not support declare it at the beginning of statement
#    if defined(__has_cpp_attribute) && (defined(__clang__) || !defined(__GNUC__) || __GNUC__ > 9)
#      if __has_cpp_attribute(likely)
#        define OPENTELEMETRY_LIKELY [[likely]]
#      endif
#    endif
#  endif
#endif

#ifndef OPENTELEMETRY_LIKELY
#  define OPENTELEMETRY_LIKELY
#endif

#if !defined(OPENTELEMETRY_UNLIKELY) && defined(__cplusplus)
// Only use unlikely with C++20
#  if __cplusplus >= 202002L
// GCC 9 has unlikely attribute but do not support declare it at the beginning of statement
#    if defined(__has_cpp_attribute) && (defined(__clang__) || !defined(__GNUC__) || __GNUC__ > 9)
#      if __has_cpp_attribute(unlikely)
#        define OPENTELEMETRY_UNLIKELY [[unlikely]]
#      endif
#    endif
#  endif
#endif

#ifndef OPENTELEMETRY_UNLIKELY
#  define OPENTELEMETRY_UNLIKELY
#endif

/// \brief Declare variable as maybe unused
/// usage:
///   OPENTELEMETRY_MAYBE_UNUSED int a;
///   class OPENTELEMETRY_MAYBE_UNUSED a;
///   OPENTELEMETRY_MAYBE_UNUSED int a();
///
#if defined(__cplusplus) && __cplusplus >= 201703L
#  define OPENTELEMETRY_MAYBE_UNUSED [[maybe_unused]]
#elif defined(__clang__)
#  define OPENTELEMETRY_MAYBE_UNUSED __attribute__((unused))
#elif defined(__GNUC__) && ((__GNUC__ >= 4) || ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 1)))
#  define OPENTELEMETRY_MAYBE_UNUSED __attribute__((unused))
#elif (defined(_MSC_VER) && _MSC_VER >= 1910) && (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#  define OPENTELEMETRY_MAYBE_UNUSED [[maybe_unused]]
#else
#  define OPENTELEMETRY_MAYBE_UNUSED
#endif

#ifndef OPENTELEMETRY_RTTI_ENABLED
#  if defined(__clang__)
#    if __has_feature(cxx_rtti)
#      define OPENTELEMETRY_RTTI_ENABLED
#    endif
#  elif defined(__GNUG__)
#    if defined(__GXX_RTTI)
#      define OPENTELEMETRY_RTTI_ENABLED
#    endif
#  elif defined(_MSC_VER)
#    if defined(_CPPRTTI)
#      define OPENTELEMETRY_RTTI_ENABLED
#    endif
#  endif
#endif

#if defined(__cplusplus) && __cplusplus >= 201402L
#  define OPENTELEMETRY_DEPRECATED [[deprecated]]
#elif defined(__clang__)
#  define OPENTELEMETRY_DEPRECATED __attribute__((deprecated))
#elif defined(__GNUC__)
#  define OPENTELEMETRY_DEPRECATED __attribute__((deprecated))
#elif defined(_MSC_VER)
#  if _MSC_VER >= 1910 && defined(_MSVC_LANG) && _MSVC_LANG >= 201703L
#    define OPENTELEMETRY_DEPRECATED [[deprecated]]
#  else
#    define OPENTELEMETRY_DEPRECATED __declspec(deprecated)
#  endif
#else
#  define OPENTELEMETRY_DEPRECATED
#endif

#if defined(__cplusplus) && __cplusplus >= 201402L
#  define OPENTELEMETRY_DEPRECATED_MESSAGE(msg) [[deprecated(msg)]]
#elif defined(__clang__)
#  define OPENTELEMETRY_DEPRECATED_MESSAGE(msg) __attribute__((deprecated(msg)))
#elif defined(__GNUC__)
#  define OPENTELEMETRY_DEPRECATED_MESSAGE(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#  if _MSC_VER >= 1910 && defined(_MSVC_LANG) && _MSVC_LANG >= 201703L
#    define OPENTELEMETRY_DEPRECATED_MESSAGE(msg) [[deprecated(msg)]]
#  else
#    define OPENTELEMETRY_DEPRECATED_MESSAGE(msg) __declspec(deprecated(msg))
#  endif
#else
#  define OPENTELEMETRY_DEPRECATED_MESSAGE(msg)
#endif

// Regex support
#if (__GNUC__ == 4 && (__GNUC_MINOR__ == 8 || __GNUC_MINOR__ == 9))
#  define OPENTELEMETRY_HAVE_WORKING_REGEX 0
#else
#  define OPENTELEMETRY_HAVE_WORKING_REGEX 1
#endif

/* clang-format off */

/**
  @page HEADER_ONLY_SINGLETON Header only singleton.

  @section ELF_SINGLETON

  For clang and gcc, the desired coding pattern is as follows.

  @verbatim
  class Foo
  {
    // (a)
    __attribute__((visibility("default")))
    // (b)
    T& get_singleton()
    {
      // (c)
      static T singleton;
      return singleton;
    }
  };
  @endverbatim

  (a) is needed when the code is build with
  @code -fvisibility="hidden" @endcode
  to ensure that all instances of (b) are visible to the linker.

  What is duplicated in the binary is @em code, in (b).

  The linker will make sure only one instance
  of all the (b) methods is used.

  (c) is a singleton implemented inside a method.

  This is very desirable, because:

  - the C++ compiler guarantees that construction
    of the variable (c) is thread safe.

  - constructors for (c) singletons are executed in code path order,
    or not at all if the singleton is never used.

  @section OTHER_SINGLETON

  For other platforms, header only singletons are not supported at this
point.

  @section CODING_PATTERN

  The coding pattern to use in the source code is as follows

  @verbatim
  class Foo
  {
    OPENTELEMETRY_API_SINGLETON
    T& get_singleton()
    {
      static T singleton;
      return singleton;
    }
  };
  @endverbatim
*/

/* clang-format on */

#if defined(__clang__)

#  define OPENTELEMETRY_API_SINGLETON __attribute__((visibility("default")))
#  define OPENTELEMETRY_LOCAL_SYMBOL __attribute__((visibility("hidden")))

#elif defined(__GNUC__)

#  define OPENTELEMETRY_API_SINGLETON __attribute__((visibility("default")))
#  define OPENTELEMETRY_LOCAL_SYMBOL __attribute__((visibility("hidden")))

#else

/* Add support for other compilers here. */

#  define OPENTELEMETRY_API_SINGLETON
#  define OPENTELEMETRY_LOCAL_SYMBOL

#endif

//
// Atomic wrappers based on compiler intrinsics for memory read/write.
// The tailing number is read/write length in bits.
//
// N.B. Compiler instrinsic is used because the usage of C++ standard library is restricted in the
// OpenTelemetry C++ API.
//
#if defined(__GNUC__)

#  define OPENTELEMETRY_ATOMIC_READ_8(ptr) __atomic_load_n(ptr, __ATOMIC_SEQ_CST)
#  define OPENTELEMETRY_ATOMIC_WRITE_8(ptr, value) __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST)

#elif defined(_MSC_VER)

#  include <intrin.h>

#  define OPENTELEMETRY_ATOMIC_READ_8(ptr) \
    static_cast<uint8_t>(_InterlockedCompareExchange8(reinterpret_cast<char *>(ptr), 0, 0))
#  define OPENTELEMETRY_ATOMIC_WRITE_8(ptr, value) \
    _InterlockedExchange8(reinterpret_cast<char *>(ptr), static_cast<char>(value))

#else
#  error port atomics read/write for the current platform
#endif

/* clang-format on */
//
// The if/elif order matters here. If both OPENTELEMETRY_BUILD_IMPORT_DLL and
// OPENTELEMETRY_BUILD_EXPORT_DLL are defined, the former takes precedence.
//
// TODO: consider define OPENTELEMETRY_EXPORT for cygwin/gcc, see below link.
// https://gcc.gnu.org/wiki/Visibility#How_to_use_the_new_C.2B-.2B-_visibility_support
//
#if defined(_MSC_VER) && defined(OPENTELEMETRY_BUILD_IMPORT_DLL)

#  define OPENTELEMETRY_EXPORT __declspec(dllimport)

#elif defined(_MSC_VER) && defined(OPENTELEMETRY_BUILD_EXPORT_DLL)

#  define OPENTELEMETRY_EXPORT __declspec(dllexport)

#else

//
// build OpenTelemetry as static library or not on Windows.
//
#  define OPENTELEMETRY_EXPORT

#endif

// OPENTELEMETRY_HAVE_EXCEPTIONS
//
// Checks whether the compiler both supports and enables exceptions. Many
// compilers support a "no exceptions" mode that disables exceptions.
//
// Generally, when OPENTELEMETRY_HAVE_EXCEPTIONS is not defined:
//
// * Code using `throw` and `try` may not compile.
// * The `noexcept` specifier will still compile and behave as normal.
// * The `noexcept` operator may still return `false`.
//
// For further details, consult the compiler's documentation.
#ifndef OPENTELEMETRY_HAVE_EXCEPTIONS
#  if defined(__clang__) && ((__clang_major__ * 100) + __clang_minor__) < 306
// Clang < 3.6
// http://releases.llvm.org/3.6.0/tools/clang/docs/ReleaseNotes.html#the-exceptions-macro
#    if defined(__EXCEPTIONS) && OPENTELEMETRY_HAVE_FEATURE(cxx_exceptions)
#      define OPENTELEMETRY_HAVE_EXCEPTIONS 1
#    endif  // defined(__EXCEPTIONS) && OPENTELEMETRY_HAVE_FEATURE(cxx_exceptions)
#  elif OPENTELEMETRY_HAVE_FEATURE(cxx_exceptions)
#    define OPENTELEMETRY_HAVE_EXCEPTIONS 1
// Handle remaining special cases and default to exceptions being supported.
#  elif !(defined(__GNUC__) && !defined(__EXCEPTIONS) && !defined(__cpp_exceptions)) && \
      !(defined(_MSC_VER) && !defined(_CPPUNWIND))
#    define OPENTELEMETRY_HAVE_EXCEPTIONS 1
#  endif
#endif
#ifndef OPENTELEMETRY_HAVE_EXCEPTIONS
#  define OPENTELEMETRY_HAVE_EXCEPTIONS 0
#endif

/*
   OPENTELEMETRY_ATTRIBUTE_LIFETIME_BOUND indicates that a resource owned by a function
   parameter or implicit object parameter is retained by the return value of the
   annotated function (or, for a parameter of a constructor, in the value of the
   constructed object). This attribute causes warnings to be produced if a
   temporary object does not live long enough.

   When applied to a reference parameter, the referenced object is assumed to be
   retained by the return value of the function. When applied to a non-reference
   parameter (for example, a pointer or a class type), all temporaries
   referenced by the parameter are assumed to be retained by the return value of
   the function.

   See also the upstream documentation:
   https://clang.llvm.org/docs/AttributeReference.html#lifetimebound
*/
#ifndef OPENTELEMETRY_ATTRIBUTE_LIFETIME_BOUND
#  if OPENTELEMETRY_HAVE_CPP_ATTRIBUTE(clang::lifetimebound)
#    define OPENTELEMETRY_ATTRIBUTE_LIFETIME_BOUND [[clang::lifetimebound]]
#  elif OPENTELEMETRY_HAVE_ATTRIBUTE(lifetimebound)
#    define OPENTELEMETRY_ATTRIBUTE_LIFETIME_BOUND __attribute__((lifetimebound))
#  else
#    define OPENTELEMETRY_ATTRIBUTE_LIFETIME_BOUND
#  endif
#endif

// OPENTELEMETRY_HAVE_MEMORY_SANITIZER
//
// MemorySanitizer (MSan) is a detector of uninitialized reads. It consists of
// a compiler instrumentation module and a run-time library.
#ifndef OPENTELEMETRY_HAVE_MEMORY_SANITIZER
#  if !defined(__native_client__) && OPENTELEMETRY_HAVE_FEATURE(memory_sanitizer)
#    define OPENTELEMETRY_HAVE_MEMORY_SANITIZER 1
#  else
#    define OPENTELEMETRY_HAVE_MEMORY_SANITIZER 0
#  endif
#endif

#if OPENTELEMETRY_HAVE_MEMORY_SANITIZER && OPENTELEMETRY_HAVE_ATTRIBUTE(no_sanitize_memory)
#  define OPENTELEMETRY_SANITIZER_NO_MEMORY \
    __attribute__((no_sanitize_memory))  // __attribute__((no_sanitize("memory")))
#else
#  define OPENTELEMETRY_SANITIZER_NO_MEMORY
#endif

// OPENTELEMETRY_HAVE_THREAD_SANITIZER
//
// ThreadSanitizer (TSan) is a fast data race detector.
#ifndef OPENTELEMETRY_HAVE_THREAD_SANITIZER
#  if defined(__SANITIZE_THREAD__)
#    define OPENTELEMETRY_HAVE_THREAD_SANITIZER 1
#  elif OPENTELEMETRY_HAVE_FEATURE(thread_sanitizer)
#    define OPENTELEMETRY_HAVE_THREAD_SANITIZER 1
#  else
#    define OPENTELEMETRY_HAVE_THREAD_SANITIZER 0
#  endif
#endif

#if OPENTELEMETRY_HAVE_THREAD_SANITIZER && OPENTELEMETRY_HAVE_ATTRIBUTE(no_sanitize_thread)
#  define OPENTELEMETRY_SANITIZER_NO_THREAD \
    __attribute__((no_sanitize_thread))  // __attribute__((no_sanitize("thread")))
#else
#  define OPENTELEMETRY_SANITIZER_NO_THREAD
#endif

// OPENTELEMETRY_HAVE_ADDRESS_SANITIZER
//
// AddressSanitizer (ASan) is a fast memory error detector.
#ifndef OPENTELEMETRY_HAVE_ADDRESS_SANITIZER
#  if defined(__SANITIZE_ADDRESS__)
#    define OPENTELEMETRY_HAVE_ADDRESS_SANITIZER 1
#  elif OPENTELEMETRY_HAVE_FEATURE(address_sanitizer)
#    define OPENTELEMETRY_HAVE_ADDRESS_SANITIZER 1
#  else
#    define OPENTELEMETRY_HAVE_ADDRESS_SANITIZER 0
#  endif
#endif

// OPENTELEMETRY_HAVE_HWADDRESS_SANITIZER
//
// Hardware-Assisted AddressSanitizer (or HWASAN) is even faster than asan
// memory error detector which can use CPU features like ARM TBI, Intel LAM or
// AMD UAI.
#ifndef OPENTELEMETRY_HAVE_HWADDRESS_SANITIZER
#  if defined(__SANITIZE_HWADDRESS__)
#    define OPENTELEMETRY_HAVE_HWADDRESS_SANITIZER 1
#  elif OPENTELEMETRY_HAVE_FEATURE(hwaddress_sanitizer)
#    define OPENTELEMETRY_HAVE_HWADDRESS_SANITIZER 1
#  else
#    define OPENTELEMETRY_HAVE_HWADDRESS_SANITIZER 0
#  endif
#endif

#if OPENTELEMETRY_HAVE_ADDRESS_SANITIZER && OPENTELEMETRY_HAVE_ATTRIBUTE(no_sanitize_address)
#  define OPENTELEMETRY_SANITIZER_NO_ADDRESS \
    __attribute__((no_sanitize_address))  // __attribute__((no_sanitize("address")))
#elif OPENTELEMETRY_HAVE_ADDRESS_SANITIZER && defined(_MSC_VER) && _MSC_VER >= 1928
#  define OPENTELEMETRY_SANITIZER_NO_ADDRESS __declspec(no_sanitize_address)
#elif OPENTELEMETRY_HAVE_HWADDRESS_SANITIZER && OPENTELEMETRY_HAVE_ATTRIBUTE(no_sanitize)
#  define OPENTELEMETRY_SANITIZER_NO_ADDRESS __attribute__((no_sanitize("hwaddress")))
#else
#  define OPENTELEMETRY_SANITIZER_NO_ADDRESS
#endif
