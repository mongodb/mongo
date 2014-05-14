/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * WARNING(schwerin): Use with extreme caution.  Prefer the AtomicWord<> types from atomic_word.h.
 *
 * The atomic_intrinsics module provides low-level atomic operations for manipulating memory.
 * Implementations are platform specific, so this file describes the interface and includes
 * the appropriate os/compiler-specific headers.
 *
 * For supported word types, the atomic_intrinsics headers provide implementations of template
 * classes of the following form:
 *
 * template <typename T> class AtomicIntrinsics {
 *     static T load(volatile const T* value);
 *     static T store(volatile T* dest, T newValue);
 *     static T compareAndSwap(volatile T* dest, T expected, T newValue);
 *     static T swap(volatile T* dest, T newValue);
 *     static T fetchAndAdd(volatile T* dest, T increment);
 * };
 *
 * All of the functions assume that the volatile T pointers are naturally aligned, and may not
 * operate as expected, if they are not so aligned.
 *
 * The behavior of the functions is analogous to the same-named member functions of the AtomicWord
 * template type in atomic_word.h.
 */

#pragma once

#if defined(_WIN32)
#include "mongo/platform/atomic_intrinsics_win32.h"
#elif defined(MONGO_HAVE_GCC_ATOMIC_BUILTINS)
#include "mongo/platform/atomic_intrinsics_gcc_atomic.h"
#elif defined(MONGO_HAVE_GCC_SYNC_BUILTINS)
#include "mongo/platform/atomic_intrinsics_gcc_sync.h"
#elif defined(__i386__) || defined(__x86_64__)
#include "mongo/platform/atomic_intrinsics_gcc_intel.h"
#else
#error "Unsupported os/compiler family"
#endif
