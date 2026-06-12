/*
 * Copyright 2009-present MongoDB, Inc.
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

#include <common-prelude.h>

#ifndef MONGO_C_DRIVER_COMMON_MACROS_PRIVATE_H
#define MONGO_C_DRIVER_COMMON_MACROS_PRIVATE_H

/* Test only assert. Is a noop unless -DENABLE_DEBUG_ASSERTIONS=ON is set
 * during configuration */
#if defined(MONGOC_ENABLE_DEBUG_ASSERTIONS) && defined(BSON_OS_UNIX)
#define MONGOC_DEBUG_ASSERT(statement) BSON_ASSERT(statement)
#else
#define MONGOC_DEBUG_ASSERT(statement) ((void)0)
#endif

#if defined(__GNUC__) && ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#define MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
#define MC_PRAGMA_DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")
#elif defined(__clang__)
#define MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("clang diagnostic push")
#define MC_PRAGMA_DIAGNOSTIC_POP _Pragma("clang diagnostic pop")
#elif defined(_MSC_VER)
#define MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("warning ( push )")
#define MC_PRAGMA_DIAGNOSTIC_POP _Pragma("warning ( pop )")
#else
#define MC_PRAGMA_DIAGNOSTIC_PUSH
#define MC_PRAGMA_DIAGNOSTIC_POP
#endif

// `MC_ENABLE_CONVERSION_WARNING_BEGIN` enables -Wconversion to check for potentially unsafe integer conversions.
// The `mcommon_in_range_*` functions can help address these warnings by ensuring a cast is within bounds.
#if defined(__GNUC__)
#define MC_ENABLE_CONVERSION_WARNING_BEGIN MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic warning \"-Wconversion\"")
#define MC_ENABLE_CONVERSION_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#elif defined(__clang__)
#define MC_ENABLE_CONVERSION_WARNING_BEGIN \
   MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("clang diagnostic warning \"-Wconversion\"")
#define MC_ENABLE_CONVERSION_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#else
#define MC_ENABLE_CONVERSION_WARNING_BEGIN
#define MC_ENABLE_CONVERSION_WARNING_END
#endif

// Disable the -Wcast-function-type-strict warning.
#define MC_DISABLE_CAST_FUNCTION_TYPE_STRICT_WARNING_BEGIN
#define MC_DISABLE_CAST_FUNCTION_TYPE_STRICT_WARNING_END
#if defined(__clang__)
#if __has_warning("-Wcast-function-type-strict")
#undef MC_DISABLE_CAST_FUNCTION_TYPE_STRICT_WARNING_BEGIN
#undef MC_DISABLE_CAST_FUNCTION_TYPE_STRICT_WARNING_END
#define MC_DISABLE_CAST_FUNCTION_TYPE_STRICT_WARNING_BEGIN \
   MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("clang diagnostic ignored \"-Wcast-function-type-strict\"")
#define MC_DISABLE_CAST_FUNCTION_TYPE_STRICT_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#endif // __has_warning("-Wcast-function-type-strict")
#endif // defined(__clang__)

#if defined(__GNUC__)
#define BEGIN_IGNORE_DEPRECATIONS \
   MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define END_IGNORE_DEPRECATIONS MC_PRAGMA_DIAGNOSTIC_POP
#elif defined(__clang__)
#define BEGIN_IGNORE_DEPRECATIONS \
   MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#define END_IGNORE_DEPRECATIONS MC_PRAGMA_DIAGNOSTIC_PUSH
#else
#define BEGIN_IGNORE_DEPRECATIONS
#define END_IGNORE_DEPRECATIONS
#endif

// Disable the -Wimplicit warning (including -Wimplicit-int and -Wimplicit-function-declaration).
#if defined(__GNUC__)
#define MC_DISABLE_IMPLICIT_WARNING_BEGIN MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic ignored \"-Wimplicit\"")
#define MC_DISABLE_IMPLICIT_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#elif defined(__clang__)
#define MC_DISABLE_IMPLICIT_WARNING_BEGIN MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("clang diagnostic ignored \"-Wimplicit\"")
#define MC_DISABLE_IMPLICIT_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#elif defined(_MSC_VER)
#define MC_DISABLE_IMPLICIT_WARNING_BEGIN MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("warning (disable : 4013 4431)")
#define MC_DISABLE_IMPLICIT_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#else
#define MC_DISABLE_IMPLICIT_WARNING_BEGIN
#define MC_DISABLE_IMPLICIT_WARNING_END
#endif

// Disable the -Wcast-qual warning
#if defined(__GNUC__)
#define MC_DISABLE_CAST_QUAL_WARNING_BEGIN MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic ignored \"-Wcast-qual\"")
#define MC_DISABLE_CAST_QUAL_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#elif defined(__clang__)
#define MC_DISABLE_CAST_QUAL_WARNING_BEGIN MC_PRAGMA_DIAGNOSTIC_PUSH _Pragma("clang diagnostic ignored \"-Wcast-qual\"")
#define MC_DISABLE_CAST_QUAL_WARNING_END MC_PRAGMA_DIAGNOSTIC_POP
#else
#define MC_DISABLE_CAST_QUAL_WARNING_BEGIN
#define MC_DISABLE_CAST_QUAL_WARNING_END
#endif

#endif /* MONGO_C_DRIVER_COMMON_MACROS_PRIVATE_H */
