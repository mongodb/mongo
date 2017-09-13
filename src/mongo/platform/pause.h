/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

/**
 * MONGO_YIELD_CORE_FOR_SMT
 * - An architecture specific processor hint to allow the processor to yield. It is designed to
 *   improve the performance of spin-wait loops.
 *
 * See src/third_party/wiredtiger/src/include/gcc.h
 */
#ifndef __MSC_VER

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

#elif defined(__aarch64__)

#define MONGO_YIELD_CORE_FOR_SMT() __asm__ volatile("yield" ::: "memory")

#elif defined(__s390x__)

#define MONGO_YIELD_CORE_FOR_SMT() __asm__ volatile("lr 0,0" ::: "memory")

#elif defined(__sparc__)

#define MONGO_YIELD_CORE_FOR_SMT() __asm__ volatile("rd %%ccr, %%g0" ::: "memory")

#else
#error "No processor pause implementation for this architecture."
#endif

#else

// On Windows, use the winnt.h YieldProcessor macro
#define MONGO_YIELD_CORE_FOR_SMT() YieldProcessor()

#endif
