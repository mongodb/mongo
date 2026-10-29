// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 *    This is heavily inspired by https://clang.llvm.org/docs/ThreadSafetyAnalysis.html.
 *
 *    To enforce a modicum of thread safety at compile time, we use the clang
 *    thread safety annotations. Mutex and mutex-like classes along with RAII-
 *    style guards must be annotated as such.
 *
 *    When an error results from clang's failure to prove that a code block is
 *    safe, or when you do something unsafe on purpose, you can use the
 *    MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS annotation. There are examples
 *    throughout the codebase.
 */

// Enable thread safety attributes only with clang.
// The attributes can be safely erased when compiling with other compilers.
#if defined(__clang__)
#define MONGO_LOCKING_ATTR(x) __attribute__((x))
#else
#define MONGO_LOCKING_ATTR(x)
#endif

#define MONGO_LOCKING_CAPABILITY(x) MONGO_LOCKING_ATTR(capability(x))

#define MONGO_LOCKING_REENTRANT_CAPABILITY MONGO_LOCKING_ATTR(reentrant_capability)

#define MONGO_LOCKING_SCOPED_CAPABILITY MONGO_LOCKING_ATTR(scoped_lockable)

#define MONGO_LOCKING_GUARDED_BY(x) MONGO_LOCKING_ATTR(guarded_by(x))

#define MONGO_LOCKING_PT_GUARDED_BY(x) MONGO_LOCKING_ATTR(pt_guarded_by(x))

#define MONGO_LOCKING_ACQUIRED_BEFORE(...) MONGO_LOCKING_ATTR(acquired_before(__VA_ARGS__))

#define MONGO_LOCKING_ACQUIRED_AFTER(...) MONGO_LOCKING_ATTR(acquired_after(__VA_ARGS__))

#define MONGO_LOCKING_REQUIRES(...) MONGO_LOCKING_ATTR(requires_capability(__VA_ARGS__))

#define MONGO_LOCKING_REQUIRES_SHARED(...) \
    MONGO_LOCKING_ATTR(requires_shared_capability(__VA_ARGS__))

#define MONGO_LOCKING_ACQUIRE(...) MONGO_LOCKING_ATTR(acquire_capability(__VA_ARGS__))

#define MONGO_LOCKING_ACQUIRE_SHARED(...) MONGO_LOCKING_ATTR(acquire_shared_capability(__VA_ARGS__))

#define MONGO_LOCKING_RELEASE(...) MONGO_LOCKING_ATTR(release_capability(__VA_ARGS__))

#define MONGO_LOCKING_RELEASE_SHARED(...) MONGO_LOCKING_ATTR(release_shared_capability(__VA_ARGS__))

#define MONGO_LOCKING_RELEASE_GENERIC(...) \
    MONGO_LOCKING_ATTR(release_generic_capability(__VA_ARGS__))

#define MONGO_LOCKING_TRY_ACQUIRE(...) MONGO_LOCKING_ATTR(try_acquire_capability(__VA_ARGS__))

#define MONGO_LOCKING_TRY_ACQUIRE_SHARED(...) \
    MONGO_LOCKING_ATTR(try_acquire_shared_capability(__VA_ARGS__))

#define MONGO_LOCKING_EXCLUDES(...) MONGO_LOCKING_ATTR(locks_excluded(__VA_ARGS__))

#define MONGO_LOCKING_ASSERT_CAPABILITY(x) MONGO_LOCKING_ATTR(assert_capability(x))

#define MONGO_LOCKING_ASSERT_SHARED_CAPABILITY(x) MONGO_LOCKING_ATTR(assert_shared_capability(x))

#define MONGO_LOCKING_RETURN_CAPABILITY(x) MONGO_LOCKING_ATTR(lock_returned(x))

#define MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS MONGO_LOCKING_ATTR(no_thread_safety_analysis)
