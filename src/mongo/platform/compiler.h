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

#pragma once

/**
 * Compiler-targeted macro definitions and utilities.
 */

#if !defined(_MSC_VER) && !defined(__GNUC__)
#error "Unsupported compiler family"
#endif

/**
 * Define clang's has_feature macro for other compilers
 * See https://clang.llvm.org/docs/LanguageExtensions.html#has-feature-and-has-extension
 */
#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

/** Allows issuing pragmas through macros. Arguments appear exactly as in a #pragma. */
#define MONGO_COMPILER_PRAGMA(p) _Pragma(#p)

#if defined(__has_cpp_attribute)
#define MONGO_COMPILER_HAS_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
#define MONGO_COMPILER_HAS_ATTRIBUTE(x) 0
#endif

/** Microsoft Visual C++ (MSVC) only. */
#if defined(_MSC_VER)
#define MONGO_COMPILER_IF_MSVC(...) __VA_ARGS__
#else
#define MONGO_COMPILER_IF_MSVC(...)
#endif

/** Clang or GCC. */
#if defined(__GNUC__)
#define MONGO_COMPILER_IF_GNUC(...) __VA_ARGS__
#else
#define MONGO_COMPILER_IF_GNUC(...)
#endif

#if defined(__clang__)
#define MONGO_COMPILER_IF_CLANG(...) MONGO_COMPILER_IF_GNUC(__VA_ARGS__)
#define MONGO_COMPILER_IF_GCC(...)
#else
#define MONGO_COMPILER_IF_CLANG(...)
#define MONGO_COMPILER_IF_GCC(...) MONGO_COMPILER_IF_GNUC(__VA_ARGS__)
#endif

/** GCC >= v14.x only. */
#if __GNUC__ >= 14
#define MONGO_COMPILER_IF_GCC14(...) MONGO_COMPILER_IF_GCC(__VA_ARGS__)
#else
#define MONGO_COMPILER_IF_GCC14(...)
#endif

/**
 * Informs the compiler that the function is cold. This can have the following effects:
 * - The function is optimized for size over speed.
 * - The function may be placed in a special cold section of the binary, away from other code.
 * - Code paths that call this function are considered implicitly unlikely.
 *
 * Cannot use new attribute syntax due to grammatical placement, especially on lambdas.
 */
#define MONGO_COMPILER_COLD_FUNCTION MONGO_COMPILER_IF_GNUC(__attribute__((__cold__)))

/**
 *   Instructs the compiler that the decorated function will not return through the normal return
 *   path. All noreturn functions are also implicitly cold since they are either run-once code
 *   executed at startup or shutdown or code that handles errors by throwing an exception.
 *
 *   Example:
 *       MONGO_COMPILER_NORETURN void myAbortFunction();
 */
#define MONGO_COMPILER_NORETURN [[noreturn]] MONGO_COMPILER_COLD_FUNCTION

/**
 *   Instructs the compiler to label the given type, variable or function as part of the
 *   exported interface of the library object under construction.
 *
 *   Usage:
 *       MONGO_COMPILER_API_EXPORT int globalSwitch;
 *       class MONGO_COMPILER_API_EXPORT ExportedType { ... };
 *       MONGO_COMPILER_API_EXPORT SomeType exportedFunction(...);
 *
 *   NOTE: Rather than using this macro directly, one typically declares another macro named
 *   for the library, which is conditionally defined to either MONGO_COMPILER_API_EXPORT or
 *   MONGO_COMPILER_API_IMPORT based on whether the compiler is currently building the library
 *   or building an object that depends on the library, respectively.  For example,
 *   MONGO_FOO_API might be defined to MONGO_COMPILER_API_EXPORT when building the MongoDB
 *   libfoo shared library, and to MONGO_COMPILER_API_IMPORT when building an application that
 *   links against that shared library.
 *
 * NOTE(schwerin): These visibility and calling-convention macro definitions assume we're not using
 * GCC/CLANG to target native Windows. If/when we decide to do such targeting, we'll need to change
 * compiler flags on Windows to make sure we use an appropriate calling convention, and configure
 * MONGO_COMPILER_API_EXPORT, MONGO_COMPILER_API_IMPORT and MONGO_COMPILER_API_CALLING_CONVENTION
 * correctly.  I believe "correctly" is the following:
 *
 *     #ifdef _WIN32
 *     #define MONGO_COMIPLER_API_EXPORT __attribute__(( __dllexport__ ))
 *     #define MONGO_COMPILER_API_IMPORT __attribute__(( __dllimport__ ))
 *     #ifdef _M_IX86
 *     #define MONGO_COMPILER_API_CALLING_CONVENTION __attribute__((__cdecl__))
 *     #else
 *     #define MONGO_COMPILER_API_CALLING_CONVENTION
 *     #endif
 *     #else // ... fall through to the definitions below.
 * NOTE: Used and retain attributes were added to prevent these symbols from getting discarded by
 * the compiler and linker when using LTO. See:
 * https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-used-function-attribute
 */
#define MONGO_COMPILER_API_EXPORT                 \
    MONGO_COMPILER_IF_MSVC(__declspec(dllexport)) \
    MONGO_COMPILER_IF_GNUC(__attribute__((__visibility__("default"), used, retain)))

/**
 *   Instructs the compiler to label the given type, variable or function as imported
 *   from another library, and not part of the library object under construction.
 *
 *   Same usage as for MONGO_COMPILER_API_EXPORT.
 */
#define MONGO_COMPILER_API_IMPORT MONGO_COMPILER_IF_MSVC(__declspec(dllimport))

/** Hide a symbol from dynamic linking. See https://gcc.gnu.org/wiki/Visibility. */
#define MONGO_COMPILER_API_HIDDEN_FUNCTION \
    MONGO_COMPILER_IF_GNUC(__attribute__((__visibility__("hidden"))))

/**
 * Overrides compiler heuristics to force a function to be inlined.
 */
#define MONGO_COMPILER_ALWAYS_INLINE MONGO_COMPILER_ALWAYS_INLINE_
#if defined(_MSC_VER)
#define MONGO_COMPILER_ALWAYS_INLINE_ [[msvc::forceinline]]
#elif MONGO_COMPILER_HAS_ATTRIBUTE(gnu::always_inline)
#define MONGO_COMPILER_ALWAYS_INLINE_ [[gnu::always_inline]]
#else
#define MONGO_COMPILER_ALWAYS_INLINE_
#endif

/**
 * Force a function to be inlined as a temporary measure to buy-back performance regressions
 * downstream of the switch to using the v5 toolchain.
 * TODO(SERVER-105707): Reevaluate each use of this macro once we have enabled LTO/PGO and
 * other post-compilation optimizations; for each, either delete the use or change it to use
 * MONGO_COMPILER_ALWAYS_INLINE, committing the forced inlining choice permanently. Then delete
 * this macro definition.
 */
#define MONGO_COMPILER_ALWAYS_INLINE_GCC14 MONGO_COMPILER_ALWAYS_INLINE

/**
 * Tells the compiler that it can assume that this line will never execute.
 * Unlike with MONGO_UNREACHABLE, there is no runtime check and reaching this
 * macro is completely undefined behavior. It should only be used where it is
 * provably impossible to reach, even in the face of adversarial inputs, but for
 * some reason the compiler cannot figure this out on its own, for example after
 * a call to a function that never returns but cannot be labeled with
 * MONGO_COMPILER_NORETURN. In almost all cases MONGO_UNREACHABLE is preferred.
 */
#define MONGO_COMPILER_UNREACHABLE          \
    MONGO_COMPILER_IF_MSVC(__assume(false)) \
    MONGO_COMPILER_IF_GNUC(__builtin_unreachable())

/**
 * Tells the compiler that it should not attempt to inline a function. This
 * option is not guaranteed to eliminate all optimizations, it only is used to
 * prevent a function from being inlined.
 */
#define MONGO_COMPILER_NOINLINE                  \
    MONGO_COMPILER_IF_MSVC(__declspec(noinline)) \
    MONGO_COMPILER_IF_GNUC([[gnu::noinline]])

/**
 * Tells the compiler that the function always returns a non-null value, potentially allowing
 * additional optimizations at call sites.
 */
#define MONGO_COMPILER_RETURNS_NONNULL MONGO_COMPILER_IF_GNUC([[gnu::returns_nonnull]])

/**
 * Tells the compiler that the function is "malloc like", in that the return value points
 * to uninitialized memory which does not alias any other valid pointers.
 */
#define MONGO_COMPILER_MALLOC                    \
    MONGO_COMPILER_IF_MSVC(__declspec(restrict)) \
    MONGO_COMPILER_IF_GNUC([[gnu::malloc]])

/**
 * Tells the compiler that the parameter indexed by `varindex`
 * provides the size of the allocated region that a "malloc like"
 * function will return a pointer to, potentially allowing static
 * analysis of use of the region when the argument to the
 * allocation function is a constant expression.
 */
#define MONGO_COMPILER_ALLOC_SIZE(varindex) MONGO_COMPILER_IF_GNUC([[gnu::alloc_size(varindex)]])

/**
 * Tells the compiler that this data member is permitted to be overlapped with other non-static
 * data members or base class subobjects of its class via subsituting in the
 * [[no_unique_address]] attribute. On Windows, the [[msvc::no_unique_address]] attribute is
 * substitued to prevent ABI-breaking changes and maintain backwards compatibility when
 * compiling with MSVC. Older versions of MSVC will not take action based on the attribute,
 * since the MSVC compiler ignores attributes it does not recognize.
 */
#define MONGO_COMPILER_NO_UNIQUE_ADDRESS MONGO_COMPILER_NO_UNIQUE_ADDRESS_
#if MONGO_COMPILER_HAS_ATTRIBUTE(msvc::no_unique_address)  // Use 'msvc::' if avail.
#define MONGO_COMPILER_NO_UNIQUE_ADDRESS_ [[msvc::no_unique_address]]
#elif MONGO_COMPILER_HAS_ATTRIBUTE(no_unique_address)
#define MONGO_COMPILER_NO_UNIQUE_ADDRESS_ [[no_unique_address]]
#else
#define MONGO_COMPILER_NO_UNIQUE_ADDRESS_
#endif

/**
 * Do not optimize the function, static variable, or class template static data member, even if
 * it is unused.
 *
 * Example:
 *     MONGO_COMPILER_USED int64_t locaInteger = 8675309;
 *     MONGO_COMPILER_USED void localFunction() {}
 *
 * See:
 * https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-used-variable-attribute
 * https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-used-function-attribute
 */
#define MONGO_COMPILER_USED MONGO_COMPILER_IF_GNUC([[gnu::used]])

/**
 * Hints to the compiler that this type is a gsl::Pointer type,
 * which will produce a compiler warning if this type is assigned
 * a temporary (xvalue) gsl::Owner type (such as std::string). This
 * matches the annotation libc++ uses for std::string_view.
 */
#define MONGO_GSL_POINTER MONGO_GSL_POINTER_
#if MONGO_COMPILER_HAS_ATTRIBUTE(gsl::Pointer)
#define MONGO_GSL_POINTER_ [[gsl::Pointer]]
#else
#define MONGO_GSL_POINTER_
#endif

/**
 * Annotating methods with [[lifetimebound]] allows the compiler to do some more
 * lifetime checking (e.g., "returned value should not outlive *this") and emit
 * warnings. See
 * https://clang.llvm.org/docs/AttributeReference.html#lifetimebound
 */
#define MONGO_COMPILER_LIFETIME_BOUND MONGO_COMPILER_LIFETIME_BOUND_
#if MONGO_COMPILER_HAS_ATTRIBUTE(lifetimebound)
#define MONGO_COMPILER_LIFETIME_BOUND_ [[lifetimebound]]
#elif MONGO_COMPILER_HAS_ATTRIBUTE(msvc::lifetimebound)
#define MONGO_COMPILER_LIFETIME_BOUND_ [[msvc::lifetimebound]]
#elif MONGO_COMPILER_HAS_ATTRIBUTE(clang::lifetimebound)
#define MONGO_COMPILER_LIFETIME_BOUND_ [[clang::lifetimebound]]
#else
#define MONGO_COMPILER_LIFETIME_BOUND_
#endif

/**
 * As an instruction cache optimization, these rearrange emitted object code so
 * that the unlikely path is moved aside. See
 * https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html#index-_005f_005fbuiltin_005fexpect
 */
#define MONGO_likely(x)             \
    MONGO_COMPILER_IF_MSVC(bool(x)) \
    MONGO_COMPILER_IF_GNUC(static_cast<bool>(__builtin_expect(static_cast<bool>(x), 1)))
#define MONGO_unlikely(x)           \
    MONGO_COMPILER_IF_MSVC(bool(x)) \
    MONGO_COMPILER_IF_GNUC(static_cast<bool>(__builtin_expect(static_cast<bool>(x), 0)))

/**
 * The `MONGO_COMPILER_DIAGNOSTIC_` macros locally control diagnostic settings.
 *    Example:
 *        MONGO_COMPILER_DIAGNOSTIC_PUSH
 *        MONGO_COMPILER_DIAGNOSTIC_IGNORED("-Wsome-warning")
 *        ...
 *        MONGO_COMPILER_DIAGNOSTIC_POP
 */

#define MONGO_COMPILER_DIAGNOSTIC_PUSH                                \
    MONGO_COMPILER_IF_GCC(MONGO_COMPILER_PRAGMA(GCC diagnostic push)) \
    MONGO_COMPILER_IF_CLANG(MONGO_COMPILER_PRAGMA(clang diagnostic push))

#define MONGO_COMPILER_DIAGNOSTIC_POP                                \
    MONGO_COMPILER_IF_GCC(MONGO_COMPILER_PRAGMA(GCC diagnostic pop)) \
    MONGO_COMPILER_IF_CLANG(MONGO_COMPILER_PRAGMA(clang diagnostic pop))

/**
 * Both GCC and clang can use this abstract macro, mostly as an implementation
 * detail. It's up to callers to know when GCC and clang disagree on the name of
 * the warning.
 */
#define MONGO_COMPILER_DIAGNOSTIC_IGNORED(w)                               \
    MONGO_COMPILER_IF_GCC(MONGO_COMPILER_PRAGMA(GCC diagnostic ignored w)) \
    MONGO_COMPILER_IF_CLANG(MONGO_COMPILER_PRAGMA(clang diagnostic ignored w))

/**
 * TODO(SERVER-102303): Delete this macro once all its uses have been removed.
 */
#define MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL(w) \
    MONGO_COMPILER_IF_GCC14(MONGO_COMPILER_DIAGNOSTIC_IGNORED(w))

/**
 * We selectively ignore `-Wstringop-overflow` on GCC 14 due to strong suspicion
 * that they are false-positives. They involve an atomic read overflowing the
 * destination, likely due to the compiler incorrectly believing they might be
 * referencing a null pointer.
 */
#define MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_READ \
    MONGO_COMPILER_IF_GCC14(MONGO_COMPILER_DIAGNOSTIC_IGNORED("-Wstringop-overflow"))

/**
 * We selectively ignore `-Wstringop-overflow` on GCC 14 due to strong suspicion
 * that they are false-positives. They involve an atomic write overflowing the
 * destination, likely due to the compiler incorrectly believing they might be
 * referencing a null pointer.
 */
#define MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_WRITE \
    MONGO_COMPILER_IF_GCC14(MONGO_COMPILER_DIAGNOSTIC_IGNORED("-Wstringop-overflow"))
