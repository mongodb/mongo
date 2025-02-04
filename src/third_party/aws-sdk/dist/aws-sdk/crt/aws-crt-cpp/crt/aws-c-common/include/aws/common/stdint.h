#ifndef AWS_COMMON_STDINT_H
#define AWS_COMMON_STDINT_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef NO_STDINT
#    include <stdint.h> /* NOLINT(fuchsia-restrict-system-includes) */
/* Android defines SIZE_MAX in limits.h, not stdint.h */
#    ifdef ANDROID
#        include <limits.h>
#    endif
#else
#    if defined(__x86_64__) || defined(_M_AMD64) || defined(__aarch64__) || defined(__ia64__) || defined(__powerpc64__)
#        define PTR_SIZE 8
#    else
#        define PTR_SIZE 4
#    endif

typedef signed char int8_t;
typedef short int int16_t;
typedef int int32_t;
#    if (PTR_SIZE == 8)
typedef long int int64_t;
#    else
typedef long long int int64_t;
#    endif /* (PTR_SIZE == 8) */

typedef unsigned char uint8_t;
typedef unsigned short int uint16_t;

typedef unsigned int uint32_t;

#    if (PTR_SIZE == 8)
typedef unsigned long int uint64_t;
#    else
typedef unsigned long long int uint64_t;
#    endif /* (PTR_SIZE == 8) */

#    if (PTR_SIZE == 8)
typedef long int intptr_t;
typedef unsigned long int uintptr_t;
#    else
typedef int intptr_t;
typedef unsigned int uintptr_t;
#    endif

#    if (PTR_SIZE == 8)
#        define __INT64_C(c) c##L
#        define __UINT64_C(c) c##UL
#    else
#        define __INT64_C(c) c##LL
#        define __UINT64_C(c) c##ULL
#    endif

#    define INT8_MIN (-128)
#    define INT16_MIN (-32767 - 1)
#    define INT32_MIN (-2147483647 - 1)
#    define INT64_MIN (-__INT64_C(9223372036854775807) - 1)
#    define INT8_MAX (127)
#    define INT16_MAX (32767)
#    define INT32_MAX (2147483647)
#    define INT64_MAX (__INT64_C(9223372036854775807))
#    define UINT8_MAX (255)
#    define UINT16_MAX (65535)
#    define UINT32_MAX (4294967295U)
#    define UINT64_MAX (__UINT64_C(18446744073709551615))

AWS_STATIC_ASSERT(sizeof(uint64_t) == 8);
AWS_STATIC_ASSERT(sizeof(uint32_t) == 4);
AWS_STATIC_ASSERT(sizeof(uint16_t) == 2);
AWS_STATIC_ASSERT(sizeof(uint8_t) == 1);
AWS_STATIC_ASSERT(sizeof(int64_t) == 8);
AWS_STATIC_ASSERT(sizeof(int32_t) == 4);
AWS_STATIC_ASSERT(sizeof(int16_t) == 2);
AWS_STATIC_ASSERT(sizeof(int8_t) == 1);
AWS_STATIC_ASSERT(sizeof(uintptr_t) == sizeof(void *));
AWS_STATIC_ASSERT(sizeof(intptr_t) == sizeof(void *));
AWS_STATIC_ASSERT(sizeof(char) == 1);
#endif /* NO_STDINT */

/**
 * @deprecated Use int64_t instead for offsets in public APIs.
 */
typedef int64_t aws_off_t;

#endif /* AWS_COMMON_STDINT_H */
