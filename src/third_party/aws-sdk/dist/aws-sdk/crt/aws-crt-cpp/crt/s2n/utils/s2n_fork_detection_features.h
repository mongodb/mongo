/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#pragma once

/* This captures Darwin specialities. This is the only APPLE flavor we care about.
 * Here we also capture various required feature test macros.
 */
#if defined(__APPLE__)
typedef struct _opaque_pthread_once_t __darwin_pthread_once_t;
typedef __darwin_pthread_once_t pthread_once_t;
    #define _DARWIN_C_SOURCE
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    /* FreeBSD requires POSIX compatibility off for its syscalls (enables __BSD_VISIBLE)
     * Without the below line, <sys/mman.h> cannot be imported (it requires __BSD_VISIBLE) */
    #undef _POSIX_C_SOURCE
#elif !defined(_GNU_SOURCE)
    #define _GNU_SOURCE
#endif

#include <sys/mman.h>

/* Not always defined for Darwin */
#if !defined(MAP_ANONYMOUS)
    #define MAP_ANONYMOUS MAP_ANON
#endif

#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
