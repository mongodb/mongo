/**
 * @file mlib/config.h
 * @brief Provides utility macros
 * @date 2024-08-29
 *
 * @note This file is intented to be standalone-includable, with no dependencies
 * other than the standard library and platform headers. This file (and other
 * `mlib` files), are for internal use only, and should not be used in any public
 * headers.
 *
 * @copyright Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef MLIB_CONFIG_H_INCLUDED
#define MLIB_CONFIG_H_INCLUDED

#ifndef _WIN32
#include <sys/param.h> // Endian detection
#endif

/**
 * @brief A function-like macro that always expands to nothing
 */
#define MLIB_NOTHING(...)

/**
 * @brief A function macro that simply expands to its arguments unchanged
 */
#define MLIB_JUST(...) __VA_ARGS__

// Paste two tokens
#ifndef _MSC_VER
#define MLIB_PASTE(A, ...) _mlibPaste1(A, __VA_ARGS__)
#else
#define MLIB_PASTE(A, ...) MLIB_JUST(_mlibPaste1(A, __VA_ARGS__))
#endif
// Paste three tokens
#define MLIB_PASTE_3(A, B, ...) MLIB_PASTE(A, MLIB_PASTE(B, __VA_ARGS__))
// Paste four tokens
#define MLIB_PASTE_4(A, B, C, ...) MLIB_PASTE(A, MLIB_PASTE_3(B, C, __VA_ARGS__))
// Paste five tokens
#define MLIB_PASTE_5(A, B, C, D, ...) MLIB_PASTE(A, MLIB_PASTE_4(B, C, D, __VA_ARGS__))
#define _mlibPaste1(A, ...) A##__VA_ARGS__

/**
 * @brief Convert the token sequence into a string after macro expansion
 */
#define MLIB_STR(...) _mlibStr(__VA_ARGS__)
#define _mlibStr(...) #__VA_ARGS__

#define MLIB_EVAL_32(...) MLIB_EVAL_16(MLIB_EVAL_16(__VA_ARGS__))
#define MLIB_EVAL_16(...) MLIB_EVAL_8(MLIB_EVAL_8(__VA_ARGS__))
#define MLIB_EVAL_8(...) MLIB_EVAL_4(MLIB_EVAL_4(__VA_ARGS__))
#define MLIB_EVAL_4(...) MLIB_EVAL_2(MLIB_EVAL_2(__VA_ARGS__))
#define MLIB_EVAL_2(...) MLIB_EVAL_1(MLIB_EVAL_1(__VA_ARGS__))
#define MLIB_EVAL_1(...) __VA_ARGS__

// clang-format off
/**
 * @brief Expand to 1 if given no arguments, otherwise 0.
 *
 * This could be done trivially using __VA_OPT__, but we need to work on
 * older compilers.
 */
#define MLIB_IS_EMPTY(...) \
    _mlibIsEmpty_1( \
        /* Expands to '1' if __VA_ARGS__ contains any top-level commas */ \
        _mlibHasComma(__VA_ARGS__), \
        /* Expands to '1' if __VA_ARGS__ begins with a parenthesis, because \
         * that will cause an "invocation" of _mlibCommaIfParens, \
         * which immediately expands to a single comma. */ \
        _mlibHasComma(_mlibCommaIfParens __VA_ARGS__), \
        /* Expands to '1' if __VA_ARGS__ expands to a function-like macro name \
         * that then expands to anything containing a top-level comma */ \
        _mlibHasComma(__VA_ARGS__ ()), \
        /* Expands to '1' if __VA_ARGS__ expands to nothing. */ \
        _mlibHasComma(_mlibCommaIfParens __VA_ARGS__ ()))
// Expand to 1 if the argument list has a comma. The weird definition is to support
// old MSVC's bad preprocessor
#define _mlibHasComma(...) \
   MLIB_JUST(_mlibPickSixteenth \
               MLIB_NOTHING("MSVC workaround") \
            (__VA_ARGS__, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, ~))
// Expands to a single comma if invoked as a function-like macro
#define _mlibCommaIfParens(...) ,

/**
 * @brief Expands to `1` if the given macro argument is a parenthesized group of
 *    tokens, otherwise `0`
 */
#define MLIB_IS_PARENTHESIZED(X) \
   _mlibHasComma(_mlibCommaIfParens X)

/**
 * @brief Pass a function-like macro name, inhibiting its expansion until the
 * next pass:
 *
 *      #define func_macro(x) x
 *
 *      MLIB_DEFERRED(func_macro)(foo)  // Expands to "func_macro(foo)", not "foo"
 */
#define MLIB_DEFERRED(MacroName) \
    /* Expand to the macro name: */ \
    MacroName \
    /*-
     * Place a separator between the function macro name and whatever comes next
     * in the file. Presumably, the next token will be the parens to invoke "MacroName",
     * but this separator inhibits its expansion unless something else comes
     * along to do another expansion pass
     */ \
    MLIB_NOTHING("[separator]")

/**
 * A helper for isEmpty(): If given (0, 0, 0, 1), expands as:
 *    - first: _mlibHasComma(_mlibIsEmptyCase_0001)
 *    -  then: _mlibHasComma(,)
 *    -  then: 1
 * Given any other aruments:
 *    - first: _mlibHasComma(_mlibIsEmptyCase_<somethingelse>)
 *    -  then: 0
 */
#define _mlibIsEmpty_1(_1, _2, _3, _4) \
    _mlibHasComma(MLIB_PASTE_5(_mlibIsEmptyCase_, _1, _2, _3, _4))
#define _mlibIsEmptyCase_0001 ,

#define MLIB_IS_NOT_EMPTY(...) MLIB_PASTE (_mlibNotEmpty_, MLIB_IS_EMPTY (__VA_ARGS__))
#define _mlibNotEmpty_1 0
#define _mlibNotEmpty_0 1
// clang-format on

/**
 * @brief If the argument expands to `0`, `false`, or nothing, expands to `0`.
 * Otherwise expands to `1`.
 */
#define MLIB_BOOLEAN(...) MLIB_IS_NOT_EMPTY(MLIB_PASTE_3(_mlib, Bool_, __VA_ARGS__))
#define _mlibBool_0
#define _mlibBool_false
#define _mlibBool_

/**
 * @brief A ternary macro. Expects three parenthesized argument lists in
 * sequence.
 *
 * If the first argument list is a truthy value, expands to the second argument
 * list. Otherwise, expands to the third argument list. The unused argument list
 * is not expanded and is discarded.
 */
#define MLIB_IF_ELSE(...) MLIB_PASTE(_mlibIfElseBranch_, MLIB_BOOLEAN(__VA_ARGS__))
#define _mlibIfElseBranch_1(...) __VA_ARGS__ MLIB_NOTHING
#define _mlibIfElseBranch_0(...) MLIB_JUST

/**
 * @brief Expands to an integer literal corresponding to the number of macro
 * arguments. Supports up to fifteen arguments.
 */
#define MLIB_ARG_COUNT(...)                 \
   MLIB_IF_ELSE(MLIB_IS_EMPTY(__VA_ARGS__)) \
   (0)(_mlibPickSixteenth(__VA_ARGS__, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#define _mlibPickSixteenth(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, ...) _16

/**
 * @brief Expand to a call expression `Prefix##_argc_N(...)`, where `N` is the
 * number of macro arguments.
 *
 * XXX: The `MLIB_JUST` forces an additional expansion pass that works around a
 * bug in the old MSVC preprocessor, but is not required in a conforming preprocessor.
 */
#define MLIB_ARGC_PICK(Prefix, ...) MLIB_JUST(MLIB_ARGC_PASTE(Prefix, __VA_ARGS__)(__VA_ARGS__))
#define MLIB_ARGC_PASTE(Prefix, ...) MLIB_PASTE_3(Prefix, _argc_, MLIB_ARG_COUNT(__VA_ARGS__))

#ifdef __cplusplus
#define mlib_is_cxx() 1
#define mlib_is_not_cxx() 0
#define MLIB_IF_CXX(...) __VA_ARGS__
#define MLIB_IF_NOT_CXX(...)
#else
#define mlib_is_cxx() 0
#define mlib_is_not_cxx() 1
#define MLIB_IF_CXX(...)
#define MLIB_IF_NOT_CXX(...) __VA_ARGS__
#endif

#define MLIB_LANG_PICK MLIB_IF_ELSE(mlib_is_not_cxx())

/**
 * @brief Use as the prefix of a braced initializer within C headers, allowing
 * the initializer to appear as a compound-init in C and an equivalent braced
 * aggregate-init in C++
 */
#define mlib_init(T) MLIB_LANG_PICK((T))(T)

/**
 * @brief Expands to `noexcept` when compiled as C++, otherwise expands to
 * nothing
 */
#define mlib_noexcept MLIB_IF_CXX(noexcept)

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#define mlib_is_little_endian() (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#elif defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN)
#define mlib_is_little_endian() (__BYTE_ORDER == __LITTLE_ENDIAN)
#elif defined(_WIN32)
#define mlib_is_little_endian() 1
#else
#error "Do not know how to detect endianness on this platform."
#endif

// clang-format off
/**
 * @brief Expands to a static assertion declaration.
 *
 * When supported, this can be replaced with `_Static_assert` or `static_assert`
 */
#define mlib_static_assert(...) MLIB_ARGC_PICK (_mlib_static_assert, __VA_ARGS__)
#define _mlib_static_assert_argc_1(Expr) \
   _mlib_static_assert_argc_2 ((Expr), "Static assertion failed")
#define _mlib_static_assert_argc_2(Expr, Msg) \
   extern int \
   MLIB_PASTE (_mlib_static_assert_placeholder, __COUNTER__)[(Expr) ? 2 : -1] \
   MLIB_IF_GNU_LIKE (__attribute__ ((unused)))
// clang-format on

#define mlib_extern_c_begin() MLIB_IF_CXX(extern "C" {) mlib_static_assert(1, "")
#define mlib_extern_c_end() MLIB_IF_CXX( \
   }) mlib_static_assert(1, "")

#ifdef __GNUC__
#define mlib_is_gnu_like() 1
#ifdef __clang__
#define mlib_is_gcc() 0
#define mlib_is_clang() 1
#else
#define mlib_is_gcc() 1
#define mlib_is_clang() 0
#endif
#define mlib_is_msvc() 0
#elif defined(_MSC_VER)
#define mlib_is_gnu_like() 0
#define mlib_is_clang() 0
#define mlib_is_gcc() 0
#define mlib_is_msvc() 1
#endif

#if defined(_WIN32)
#define mlib_is_win32() 1
#define mlib_is_unix() 0
#else
#define mlib_is_unix() 1
#define mlib_is_win32() 0
#endif

#define MLIB_IF_CLANG(...) MLIB_IF_ELSE(mlib_is_clang())(__VA_ARGS__)(MLIB_NOTHING(#__VA_ARGS__))
#define MLIB_IF_GCC(...) MLIB_IF_ELSE(mlib_is_gcc())(__VA_ARGS__)(MLIB_NOTHING(#__VA_ARGS__))
#define MLIB_IF_GNU_LIKE(...) MLIB_IF_GCC(__VA_ARGS__) MLIB_IF_CLANG(__VA_ARGS__) MLIB_NOTHING(#__VA_ARGS__)
#define MLIB_IF_UNIX_LIKE(...) MLIB_IF_ELSE(mlib_is_unix())(__VA_ARGS__)(MLIB_NOTHING(#__VA_ARGS__))

// note: Bug on GCC preprocessor prevents us from using if/else trick to omit MSVC code
#if mlib_is_msvc()
#define MLIB_IF_MSVC(...) __VA_ARGS__
#define mlib_pragma(...) __pragma(__VA_ARGS__) mlib_static_assert(1, "")
#else
#define MLIB_IF_MSVC(...) MLIB_NOTHING(#__VA_ARGS__)
#define mlib_pragma(...) _Pragma(#__VA_ARGS__) mlib_static_assert(1, "")
#endif

#define MLIB_PRAGMA_IF_CLANG(...) MLIB_IF_CLANG(_Pragma(#__VA_ARGS__))
#define MLIB_PRAGMA_IF_GCC(...) MLIB_IF_GCC(_Pragma(#__VA_ARGS__))
#define MLIB_PRAGMA_IF_GNU_LIKE(...) MLIB_IF_GNU_LIKE(_Pragma(#__VA_ARGS__))
#define MLIB_PRAGMA_IF_UNIX_LIKE(...) MLIB_IF_UNIX_LIKE(_Pragma(#__VA_ARGS__))
#define MLIB_PRAGMA_IF_MSVC(...) MLIB_IF_MSVC(__pragma(__VA_ARGS__))

#define MLIB_FUNC MLIB_IF_GNU_LIKE(__func__) MLIB_IF_MSVC(__FUNCTION__)

#define mlib_diagnostic_push()                         \
   MLIB_IF_GNU_LIKE(mlib_pragma(GCC diagnostic push);) \
   MLIB_IF_MSVC(mlib_pragma(warning(push));)           \
   mlib_static_assert(1, "")

#define mlib_diagnostic_pop()                         \
   MLIB_IF_GNU_LIKE(mlib_pragma(GCC diagnostic pop);) \
   MLIB_IF_MSVC(mlib_pragma(warning(pop));)           \
   mlib_static_assert(1, "")

#define mlib_gcc_warning_disable(Warning)                    \
   MLIB_IF_GCC(mlib_pragma(GCC diagnostic ignored Warning);) \
   mlib_static_assert(1, "")

#define mlib_gnu_warning_disable(Warning)                         \
   MLIB_IF_GNU_LIKE(mlib_pragma(GCC diagnostic ignored Warning);) \
   mlib_static_assert(1, "")

#define mlib_msvc_warning(...)                      \
   MLIB_IF_MSVC(mlib_pragma(warning(__VA_ARGS__));) \
   mlib_static_assert(1, "")

/**
 * @brief Attribute macro that forces the function to be inlined at all call sites.
 *
 * Don't use this unless you really know that you need it, lest you generate code
 * bloat when the compiler's heuristics would do a better job.
 */
#define mlib_always_inline MLIB_IF_GNU_LIKE(__attribute__((always_inline)) inline) MLIB_IF_MSVC(__forceinline)

// Annotate a variable as thread-local
#define mlib_thread_local MLIB_IF_GNU_LIKE(__thread) MLIB_IF_MSVC(__declspec(thread))

// Annotate an entiry that might be unused
#define mlib_maybe_unused MLIB_IF_GNU_LIKE(__attribute__((unused)))

// clang-format off
/**
 * @brief Expand to `1` if the current build configuration matches the given token.
 *
 * If the token is not a known valid build configuration, generates a compilation
 * error (check your spelling!)
 *
 * Requires that `_MLIB_BUILD_CONFIG` is defined, otherwise always expands to `0`
 */
#define mlib_build_config_is(Config) \
   /* If `Config` is a recognized config, this line will disappear, */ \
   /* other wise it will be a "call to undefined macro": */ \
   MLIB_PASTE_4 (_mlibTestBuildConfig_, Config, _, Config) () \
   /* If `Config` is the same token as `_MLIB_BUILD_CONFIG`, this will */ \
   /* expand to `1`, otherwise it will expand to `0` */ \
   MLIB_IS_EMPTY (MLIB_PASTE_4 (_mlibTestBuildConfig_, Config, _, _MLIB_BUILD_CONFIG) ())
// clang-format on
// Known build configurations:
#define _mlibTestBuildConfig_Release_Release()
#define _mlibTestBuildConfig_Debug_Debug()
#define _mlibTestBuildConfig_RelWithDebInfo_RelWithDebInfo()
#define _mlibTestBuildConfig_MinSizeRel_MinSizeRel()

/**
 * @brief Emit a _Pragma that will disable warnings about the use of deprecated entities.
 */
#define mlib_disable_deprecation_warnings()               \
   mlib_gnu_warning_disable("-Wdeprecated-declarations"); \
   mlib_msvc_warning(disable : 4996)

/**
 * @brief Function-like macro that expands to `1` if we are certain that we are
 * compiling with optimizations enabled.
 *
 * This may yield `0` if we cannot determine whether optimization is turned on.
 *
 * This macro should be used with care, as different translation units can see different values,
 * but still be linked together in the final program. Beware generating ODR violations.
 */
#define mlib_is_optimized_build() _mlibIsOptimizedBuild()

#if mlib_build_config_is(Release) || mlib_build_config_is(RelWithDebInfo) || mlib_build_config_is(MinSizeRel) || \
   (defined(__OPTIMIZE__) && __OPTIMIZE__)
// Preproc definition __OPTIMIZE__set by GCC ang Clang when the optimizer is enabled.
// MSVC has no such definition, so we rely on CMake to tell us when we are compiling in release mode
#define _mlibIsOptimizedBuild() 1
#else
#define _mlibIsOptimizedBuild() 0
#endif

#if mlib_is_gnu_like()
#define mlib_have_typeof() 1
#elif defined _MSC_VER && _MSC_VER >= 1939 && !__cplusplus
// We can __typeof__ in MSVC 19.39+
#define mlib_have_typeof() 1
#else
#define mlib_have_typeof() 0
#endif

/**
 * @brief Equivalent to C23's `typeof()`, if it is supported by the current compiler.
 *
 * This expands to `__typeof__`, which is supported even on newer MSVC compilers,
 * even when not in C23 mode.
 */
#define mlib_typeof(...) MLIB_IF_ELSE(mlib_have_typeof())(__typeof__)(__mlib_typeof_is_not_supported)(__VA_ARGS__)

/**
 * @brief Disable warnings for constant conditional expressions.
 */
#define mlib_disable_constant_conditional_expression_warnings() mlib_msvc_warning(disable : 4127)

/**
 * @brief Disable warnings for potentially unused parameters.
 */
#define mlib_disable_unused_parameter_warnings()                     \
   MLIB_IF_GNU_LIKE(mlib_gnu_warning_disable("-Wunused-parameter");) \
   MLIB_IF_MSVC(mlib_msvc_warning(disable : 4100);) mlib_static_assert(1, "")

#if mlib_is_clang()
#define mlib_printf_attribute(f, v) __attribute__((format(printf, f, v)))
#elif mlib_is_gcc()
#define mlib_printf_attribute(f, v) __attribute__((format(gnu_printf, f, v)))
#else
#define mlib_printf_attribute(f, v)
#endif

/**
 * @brief Annotate a boolean expression as "likely to be true" to guide the optimizer.
 * Use this very sparingly.
 */
#define mlib_likely(...) MLIB_IF_ELSE(mlib_is_gnu_like())(__builtin_expect(!!(__VA_ARGS__), 1))((__VA_ARGS__))
/**
 * @brief Annotate a boolean expression as "likely to be untrue" to guide the optimizer.
 * Use this very sparingly.
 */
#define mlib_unlikely(...) MLIB_IF_ELSE(mlib_is_gnu_like())(__builtin_expect(!!(__VA_ARGS__), 0))((__VA_ARGS__))

#endif // MLIB_CONFIG_H_INCLUDED
