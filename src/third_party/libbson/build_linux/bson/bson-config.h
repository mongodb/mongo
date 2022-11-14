/*
 * Copyright 2018-present MongoDB, Inc.
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

#if !defined(BSON_INSIDE) && !defined(BSON_COMPILATION)
#error "Only <bson/bson.h> can be included directly."
#endif

#ifndef BSON_CONFIG_H
#define BSON_CONFIG_H

/*
 * Define to 1234 for Little Endian, 4321 for Big Endian.
 */
#define BSON_BYTE_ORDER 1234


/*
 * Define to 1 if you have stdbool.h
 */
#define BSON_HAVE_STDBOOL_H 1
#if BSON_HAVE_STDBOOL_H != 1
# undef BSON_HAVE_STDBOOL_H
#endif


/*
 * Define to 1 for POSIX-like systems, 2 for Windows.
 */
#define BSON_OS 1


/*
 * Define to 1 if we have access to GCC 32-bit atomic builtins.
 * While this requires GCC 4.1+ in most cases, it is also architecture
 * dependent. For example, some PPC or ARM systems may not have it even
 * if it is a recent GCC version.
 */
#define BSON_HAVE_ATOMIC_32_ADD_AND_FETCH 1
#if BSON_HAVE_ATOMIC_32_ADD_AND_FETCH != 1
# undef BSON_HAVE_ATOMIC_32_ADD_AND_FETCH
#endif

/*
 * Similarly, define to 1 if we have access to GCC 64-bit atomic builtins.
 */
#define BSON_HAVE_ATOMIC_64_ADD_AND_FETCH 1
#if BSON_HAVE_ATOMIC_64_ADD_AND_FETCH != 1
# undef BSON_HAVE_ATOMIC_64_ADD_AND_FETCH
#endif


/*
 * Define to 1 if you have clock_gettime() available.
 */
#define BSON_HAVE_CLOCK_GETTIME 1
#if BSON_HAVE_CLOCK_GETTIME != 1
# undef BSON_HAVE_CLOCK_GETTIME
#endif


/*
 * Define to 1 if you have strings.h available on your platform.
 */
#define BSON_HAVE_STRINGS_H 1
#if BSON_HAVE_STRINGS_H != 1
# undef BSON_HAVE_STRINGS_H
#endif


/*
 * Define to 1 if you have strnlen available on your platform.
 */
#define BSON_HAVE_STRNLEN 1
#if BSON_HAVE_STRNLEN != 1
# undef BSON_HAVE_STRNLEN
#endif


/*
 * Define to 1 if you have snprintf available on your platform.
 */
#define BSON_HAVE_SNPRINTF 1
#if BSON_HAVE_SNPRINTF != 1
# undef BSON_HAVE_SNPRINTF
#endif


/*
 * Define to 1 if you have gmtime_r available on your platform.
 */
#define BSON_HAVE_GMTIME_R 1
#if BSON_HAVE_GMTIME_R != 1
# undef BSON_HAVE_GMTIME_R
#endif


/*
 * Define to 1 if you have reallocf available on your platform.
 */
#define BSON_HAVE_REALLOCF 0
#if BSON_HAVE_REALLOCF != 1
# undef BSON_HAVE_REALLOCF
#endif


/*
 * Define to 1 if you have struct timespec available on your platform.
 */
#define BSON_HAVE_TIMESPEC 1
#if BSON_HAVE_TIMESPEC != 1
# undef BSON_HAVE_TIMESPEC
#endif


/*
 * Define to 1 if you want extra aligned types in libbson
 */
#define BSON_EXTRA_ALIGN 1
#if BSON_EXTRA_ALIGN != 1
# undef BSON_EXTRA_ALIGN
#endif


/*
 * Define to 1 if you have SYS_gettid syscall
 */
#define BSON_HAVE_SYSCALL_TID 1
#if BSON_HAVE_SYSCALL_TID != 1
# undef BSON_HAVE_SYSCALL_TID
#endif

#define BSON_HAVE_RAND_R 1
#if BSON_HAVE_RAND_R != 1
# undef BSON_HAVE_RAND_R
#endif

#define BSON_HAVE_STRLCPY 0
#if BSON_HAVE_STRLCPY != 1
# undef BSON_HAVE_STRLCPY
#endif

#endif /* BSON_CONFIG_H */
