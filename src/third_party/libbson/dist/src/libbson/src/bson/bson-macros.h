/*
 * Copyright 2013 MongoDB, Inc.
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

#include "bson-prelude.h"


#ifndef BSON_MACROS_H
#define BSON_MACROS_H


#include <stdio.h>

#ifdef __cplusplus
#include <algorithm>
#endif

#include "bson-config.h"


#if BSON_OS == 1
#define BSON_OS_UNIX
#elif BSON_OS == 2
#define BSON_OS_WIN32
#else
#error "Unknown operating system."
#endif


#ifdef __cplusplus
#define BSON_BEGIN_DECLS extern "C" {
#define BSON_END_DECLS }
#else
#define BSON_BEGIN_DECLS
#define BSON_END_DECLS
#endif


#if defined(__GNUC__)
#define BSON_GNUC_CHECK_VERSION(major, minor) \
   ((__GNUC__ > (major)) ||                   \
    ((__GNUC__ == (major)) && (__GNUC_MINOR__ >= (minor))))
#else
#define BSON_GNUC_CHECK_VERSION(major, minor) 0
#endif


#if defined(__GNUC__)
#define BSON_GNUC_IS_VERSION(major, minor) \
   ((__GNUC__ == (major)) && (__GNUC_MINOR__ == (minor)))
#else
#define BSON_GNUC_IS_VERSION(major, minor) 0
#endif


/* Decorate public functions:
 * - if BSON_STATIC, we're compiling a program that uses libbson as a static
 *   library, don't decorate functions
 * - else if BSON_COMPILATION, we're compiling a static or shared libbson, mark
 *   public functions for export from the shared lib (which has no effect on
 *   the static lib)
 * - else, we're compiling a program that uses libbson as a shared library,
 *   mark public functions as DLL imports for Microsoft Visual C
 */

#ifdef _MSC_VER
/*
 * Microsoft Visual C
 */
#ifdef BSON_STATIC
#define BSON_API
#elif defined(BSON_COMPILATION)
#define BSON_API __declspec(dllexport)
#else
#define BSON_API __declspec(dllimport)
#endif
#define BSON_CALL __cdecl

#elif defined(__GNUC__)
/*
 * GCC
 */
#ifdef BSON_STATIC
#define BSON_API
#elif defined(BSON_COMPILATION)
#define BSON_API __attribute__ ((visibility ("default")))
#else
#define BSON_API
#endif
#define BSON_CALL

#else
/*
 * Other compilers
 */
#define BSON_API
#define BSON_CALL

#endif

#define BSON_EXPORT(type) BSON_API type BSON_CALL


#ifdef MIN
#define BSON_MIN MIN
#elif defined(__cplusplus)
#define BSON_MIN(a, b) ((std::min) (a, b))
#elif defined(_MSC_VER)
#define BSON_MIN(a, b) ((a) < (b) ? (a) : (b))
#else
#define BSON_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif


#ifdef MAX
#define BSON_MAX MAX
#elif defined(__cplusplus)
#define BSON_MAX(a, b) ((std::max) (a, b))
#elif defined(_MSC_VER)
#define BSON_MAX(a, b) ((a) > (b) ? (a) : (b))
#else
#define BSON_MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif


#ifdef ABS
#define BSON_ABS ABS
#else
#define BSON_ABS(a) (((a) < 0) ? ((a) * -1) : (a))
#endif

#if __STDC_VERSION__ >= 201112L
#define BSON_ALIGNOF(expr) _Alignof(expr)
#else
#if defined(_MSC_VER)
#define BSON_ALIGNOF(expr) __alignof(expr)
#else
#define BSON_ALIGNOF(expr) __alignof__(expr)
#endif
#endif // __STDC_VERSION__ >= 201112L

#ifdef _MSC_VER
// __declspec (align (_N)) only permits integer literals as _N.
#ifdef _WIN64
#define BSON_ALIGN_OF_PTR 8
#else
#define BSON_ALIGN_OF_PTR 4
#endif
#else
#define BSON_ALIGN_OF_PTR (BSON_ALIGNOF (void *))
#endif

#ifdef BSON_EXTRA_ALIGN
#if defined(_MSC_VER)
#define BSON_ALIGNED_BEGIN(_N) __declspec(align (_N))
#define BSON_ALIGNED_END(_N)
#else
#define BSON_ALIGNED_BEGIN(_N)
#define BSON_ALIGNED_END(_N) __attribute__ ((aligned (_N)))
#endif
#else
#if defined(_MSC_VER)
#define BSON_ALIGNED_BEGIN(_N) __declspec(align (BSON_ALIGN_OF_PTR))
#define BSON_ALIGNED_END(_N)
#else
#define BSON_ALIGNED_BEGIN(_N)
#define BSON_ALIGNED_END(_N) \
   __attribute__ ((          \
      aligned ((_N) > BSON_ALIGN_OF_PTR ? BSON_ALIGN_OF_PTR : (_N))))
#endif
#endif


#define bson_str_empty(s) (!s[0])
#define bson_str_empty0(s) (!s || !s[0])


#if defined(_MSC_VER)
#define BSON_FUNC __FUNCTION__
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ < 199901L
#define BSON_FUNC __FUNCTION__
#else
#define BSON_FUNC __func__
#endif

#define BSON_ASSERT(test)                                  \
   do {                                                    \
      if (!(BSON_LIKELY (test))) {                         \
         fprintf (stderr,                                  \
                  "%s:%d %s(): precondition failed: %s\n", \
                  __FILE__,                                \
                  __LINE__,                                \
                  BSON_FUNC,                               \
                  #test);                                  \
         abort ();                                         \
      }                                                    \
   } while (0)

/**
 * @brief Assert the expression `Assertion`, and evaluates to `Value` on
 * success.
 */
#define BSON_ASSERT_INLINE(Assertion, Value)                              \
   ((void) ((Assertion) ? (0)                                             \
                        : ((fprintf (stderr,                              \
                                     "%s:%d %s(): Assertion '%s' failed", \
                                     __FILE__,                            \
                                     __LINE__,                            \
                                     BSON_FUNC,                           \
                                     #Assertion),                         \
                            abort ()),                                    \
                           0)),                                           \
    Value)

/**
 * @brief Assert that the given pointer is non-NULL, while also evaluating to
 * that pointer.
 *
 * Can be used to inline assertions with a pointer dereference:
 *
 * ```
 * foo* f = get_foo();
 * bar* b = BSON_ASSERT_PTR_INLINE(f)->bar_value;
 * ```
 */
#define BSON_ASSERT_PTR_INLINE(Pointer) \
   BSON_ASSERT_INLINE ((Pointer) != NULL, (Pointer))

/* Used for asserting parameters to provide a more precise error message */
#define BSON_ASSERT_PARAM(param)                                         \
   do {                                                                  \
      if ((BSON_UNLIKELY (param == NULL))) {                             \
         fprintf (stderr,                                                \
                  "The parameter: %s, in function %s, cannot be NULL\n", \
                  #param,                                                \
                  BSON_FUNC);                                            \
         abort ();                                                       \
      }                                                                  \
   } while (0)

/* obsolete macros, preserved for compatibility */
#define BSON_STATIC_ASSERT(s) BSON_STATIC_ASSERT_ (s, __LINE__)
#define BSON_STATIC_ASSERT_JOIN(a, b) BSON_STATIC_ASSERT_JOIN2 (a, b)
#define BSON_STATIC_ASSERT_JOIN2(a, b) a##b
#define BSON_STATIC_ASSERT_(s, l)                             \
   typedef char BSON_STATIC_ASSERT_JOIN (static_assert_test_, \
                                         __LINE__)[(s) ? 1 : -1]

/* modern macros */
#define BSON_STATIC_ASSERT2(_name, _s) \
   BSON_STATIC_ASSERT2_ (_s, __LINE__, _name)
#define BSON_STATIC_ASSERT_JOIN3(_a, _b, _name) \
   BSON_STATIC_ASSERT_JOIN4 (_a, _b, _name)
#define BSON_STATIC_ASSERT_JOIN4(_a, _b, _name) _a##_b##_name
#define BSON_STATIC_ASSERT2_(_s, _l, _name) \
   typedef char BSON_STATIC_ASSERT_JOIN3 (  \
      static_assert_test_, __LINE__, _name)[(_s) ? 1 : -1]


#if defined(__GNUC__)
#define BSON_GNUC_PURE __attribute__ ((pure))
#define BSON_GNUC_WARN_UNUSED_RESULT __attribute__ ((warn_unused_result))
#else
#define BSON_GNUC_PURE
#define BSON_GNUC_WARN_UNUSED_RESULT
#endif


#if BSON_GNUC_CHECK_VERSION(4, 0) && !defined(_WIN32)
#define BSON_GNUC_NULL_TERMINATED __attribute__ ((sentinel))
#define BSON_GNUC_INTERNAL __attribute__ ((visibility ("hidden")))
#else
#define BSON_GNUC_NULL_TERMINATED
#define BSON_GNUC_INTERNAL
#endif


#if defined(__GNUC__)
#define BSON_LIKELY(x) __builtin_expect (!!(x), 1)
#define BSON_UNLIKELY(x) __builtin_expect (!!(x), 0)
#else
#define BSON_LIKELY(v) v
#define BSON_UNLIKELY(v) v
#endif


#if defined(__clang__)
#define BSON_GNUC_PRINTF(f, v) __attribute__ ((format (printf, f, v)))
#elif BSON_GNUC_CHECK_VERSION(4, 4)
#define BSON_GNUC_PRINTF(f, v) __attribute__ ((format (gnu_printf, f, v)))
#else
#define BSON_GNUC_PRINTF(f, v)
#endif


#if defined(__LP64__) || defined(_LP64)
#define BSON_WORD_SIZE 64
#else
#define BSON_WORD_SIZE 32
#endif


#if defined(_MSC_VER)
#define BSON_INLINE __inline
#else
#define BSON_INLINE __inline__
#endif


#ifdef _MSC_VER
#define BSON_ENSURE_ARRAY_PARAM_SIZE(_n)
#define BSON_TYPEOF decltype
#else
#define BSON_ENSURE_ARRAY_PARAM_SIZE(_n) static(_n)
#define BSON_TYPEOF typeof
#endif


#if BSON_GNUC_CHECK_VERSION(3, 1)
#define BSON_GNUC_DEPRECATED __attribute__ ((__deprecated__))
#else
#define BSON_GNUC_DEPRECATED
#endif

#define BSON_CONCAT_IMPL(a, ...) a##__VA_ARGS__
#define BSON_CONCAT(a, ...) BSON_CONCAT_IMPL (a, __VA_ARGS__)
#define BSON_CONCAT3(a, b, c) BSON_CONCAT (a, BSON_CONCAT (b, c))
#define BSON_CONCAT4(a, b, c, d) \
   BSON_CONCAT (BSON_CONCAT (a, b), BSON_CONCAT (c, d))

#if BSON_GNUC_CHECK_VERSION(4, 5)
#define BSON_GNUC_DEPRECATED_FOR(f) \
   __attribute__ ((deprecated ("Use " #f " instead")))
#else
#define BSON_GNUC_DEPRECATED_FOR(f) BSON_GNUC_DEPRECATED
#endif

/**
 * @brief String-ify the given argument
 */
#define BSON_STR(...) #__VA_ARGS__

/**
 * @brief Mark the attached declared entity as "possibly-unused."
 *
 * Does nothing on MSVC.
 */
#if defined(__GNUC__) || defined(__clang__)
#define BSON_MAYBE_UNUSED __attribute__ ((unused))
#else
#define BSON_MAYBE_UNUSED /* Nothing for other compilers */
#endif

/**
 * @brief Mark a point in the code as unreachable. If the point is reached, the
 * program will abort with an error message.
 *
 * @param What A string to include in the error message if this point is ever
 * executed.
 */
#define BSON_UNREACHABLE(What)                               \
   do {                                                      \
      fprintf (stderr,                                       \
               "%s:%d %s(): Unreachable code reached: %s\n", \
               __FILE__,                                     \
               __LINE__,                                     \
               BSON_FUNC,                                    \
               What);                                        \
      abort ();                                              \
   } while (0)

/**
 * @brief Silence warnings for deliberately unused variables or parameters.
 *
 * @param expr An unused variable or parameter.
 *
 */
#define BSON_UNUSED(expr) \
   do {                   \
      (void) (expr);      \
   } while (0)

#endif /* BSON_MACROS_H */
