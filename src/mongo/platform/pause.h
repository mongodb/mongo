// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * MONGO_YIELD_CORE_FOR_SMT
 * - An architecture specific processor hint to allow the processor to yield. It is designed to
 *   improve the performance of spin-wait loops.
 *
 * See src/third_party/wiredtiger/src/include/gcc.h
 */
#ifndef _MSC_VER

#if defined(x86_64) || defined(__x86_64__)

#include <xmmintrin.h>

/* Pause instruction to prevent excess processor bus usage */
#define MONGO_YIELD_CORE_FOR_SMT() _mm_pause()

#elif defined(i386) || defined(__i386__)

#include <xmmintrin.h>

#define MONGO_YIELD_CORE_FOR_SMT() _mm_pause()

#elif defined(__PPC64__) || defined(PPC64)

/* ori 0,0,0 is the PPC64 noop instruction */
#define MONGO_YIELD_CORE_FOR_SMT() __asm__ volatile("ori 0,0,0" ::: "memory")

#elif defined(__aarch64__) || defined(__arm__)

/* See https://jira.mongodb.org/browse/WT-6872 for details on using `isb` instead of `yield`. */
#define MONGO_YIELD_CORE_FOR_SMT() __asm__ volatile("isb" ::: "memory")

#elif defined(__s390x__)

#define MONGO_YIELD_CORE_FOR_SMT() __asm__ volatile("lr 0,0" ::: "memory")

#elif defined(__sparc__)

#define MONGO_YIELD_CORE_FOR_SMT() __asm__ volatile("rd %%ccr, %%g0" ::: "memory")

#elif defined(__EMSCRIPTEN__)

// TODO: What should this be?
#define MONGO_YIELD_CORE_FOR_SMT()

#else
#error "No processor pause implementation for this architecture."
#endif

#else

// On Windows, use the winnt.h YieldProcessor macro
#define MONGO_YIELD_CORE_FOR_SMT() YieldProcessor()

#endif
