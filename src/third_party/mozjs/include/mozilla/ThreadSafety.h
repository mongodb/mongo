// Note: the file is largely imported directly from WebRTC upstream, so
// comments may not completely apply to Mozilla's usage.
//
// Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
//
// Borrowed from
// https://code.google.com/p/gperftools/source/browse/src/base/thread_annotations.h
// but adapted for clang attributes instead of the gcc.
//
// This header file contains the macro definitions for thread safety
// annotations that allow the developers to document the locking policies
// of their multi-threaded code. The annotations can also help program
// analysis tools to identify potential thread safety issues.

#ifndef mozilla_ThreadSafety_h
#define mozilla_ThreadSafety_h
#include "mozilla/Attributes.h"

#if defined(__clang__) && (__clang_major__ >= 11) && !defined(SWIG)
#  define MOZ_THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
// Allow for localized suppression of thread-safety warnings; finer-grained
// than MOZ_NO_THREAD_SAFETY_ANALYSIS
#  define MOZ_PUSH_IGNORE_THREAD_SAFETY \
    _Pragma("GCC diagnostic push")      \
        _Pragma("GCC diagnostic ignored \"-Wthread-safety\"")
#  define MOZ_POP_THREAD_SAFETY _Pragma("GCC diagnostic pop")

#else
#  define MOZ_THREAD_ANNOTATION_ATTRIBUTE__(x)  // no-op
#  define MOZ_PUSH_IGNORE_THREAD_SAFETY
#  define MOZ_POP_THREAD_SAFETY
#endif

// Document if a shared variable/field needs to be protected by a lock.
// MOZ_GUARDED_BY allows the user to specify a particular lock that should be
// held when accessing the annotated variable, while MOZ_GUARDED_VAR only
// indicates a shared variable should be guarded (by any lock). MOZ_GUARDED_VAR
// is primarily used when the client cannot express the name of the lock.
#define MOZ_GUARDED_BY(x) MOZ_THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#define MOZ_GUARDED_VAR MOZ_THREAD_ANNOTATION_ATTRIBUTE__(guarded_var)

// Document if the memory location pointed to by a pointer should be guarded
// by a lock when dereferencing the pointer. Similar to MOZ_GUARDED_VAR,
// MOZ_PT_GUARDED_VAR is primarily used when the client cannot express the
// name of the lock. Note that a pointer variable to a shared memory location
// could itself be a shared variable. For example, if a shared global pointer
// q, which is guarded by mu1, points to a shared memory location that is
// guarded by mu2, q should be annotated as follows:
//     int *q MOZ_GUARDED_BY(mu1) MOZ_PT_GUARDED_BY(mu2);
#define MOZ_PT_GUARDED_BY(x) MOZ_THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
#define MOZ_PT_GUARDED_VAR MOZ_THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_var)

// Document the acquisition order between locks that can be held
// simultaneously by a thread. For any two locks that need to be annotated
// to establish an acquisition order, only one of them needs the annotation.
// (i.e. You don't have to annotate both locks with both MOZ_ACQUIRED_AFTER
// and MOZ_ACQUIRED_BEFORE.)
#define MOZ_ACQUIRED_AFTER(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))
#define MOZ_ACQUIRED_BEFORE(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))

// The following three annotations document the lock requirements for
// functions/methods.

// Document if a function expects certain locks to be held before it is called
#define MOZ_REQUIRES(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(__VA_ARGS__))

#define MOZ_REQUIRES_SHARED(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(shared_locks_required(__VA_ARGS__))

// Document the locks acquired in the body of the function. These locks
// cannot be held when calling this function (as google3's Mutex locks are
// non-reentrant).
#define MOZ_EXCLUDES(x) MOZ_THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(x))

// Document the lock the annotated function returns without acquiring it.
#define MOZ_RETURN_CAPABILITY(x) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))

// Document if a class/type is a lockable type (such as the Mutex class).
#define MOZ_CAPABILITY(x) MOZ_THREAD_ANNOTATION_ATTRIBUTE__(capability(x))

// Document if a class is a scoped lockable type (such as the MutexLock class).
#define MOZ_SCOPED_CAPABILITY MOZ_THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)

// The following annotations specify lock and unlock primitives.
#define MOZ_CAPABILITY_ACQUIRE(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_lock_function(__VA_ARGS__))

#define MOZ_EXCLUSIVE_RELEASE(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))

#define MOZ_ACQUIRE_SHARED(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(shared_lock_function(__VA_ARGS__))

#define MOZ_TRY_ACQUIRE(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_trylock_function(__VA_ARGS__))

#define MOZ_SHARED_TRYLOCK_FUNCTION(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(shared_trylock_function(__VA_ARGS__))

#define MOZ_CAPABILITY_RELEASE(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(unlock_function(__VA_ARGS__))

// An escape hatch for thread safety analysis to ignore the annotated function.
#define MOZ_NO_THREAD_SAFETY_ANALYSIS \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

// Newer capabilities
#define MOZ_ASSERT_CAPABILITY(x) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(x))

#define MOZ_ASSERT_SHARED_CAPABILITY(x) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(x))

// Additions from current clang assertions.
// Note: new-style definitions, since these didn't exist in the old style
#define MOZ_RELEASE_SHARED(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))

#define MOZ_RELEASE_GENERIC(...) \
  MOZ_THREAD_ANNOTATION_ATTRIBUTE__(release_generic_capability(__VA_ARGS__))

// Mozilla additions:

// AutoUnlock is supported by clang currently, but oddly you must use
// MOZ_EXCLUSIVE_RELEASE() for both the RAII constructor *and* the destructor.
// This hides the ugliness until they fix it upstream.
#define MOZ_SCOPED_UNLOCK_RELEASE(...) MOZ_EXCLUSIVE_RELEASE(__VA_ARGS__)
#define MOZ_SCOPED_UNLOCK_REACQUIRE(...) MOZ_EXCLUSIVE_RELEASE(__VA_ARGS__)

#endif /* mozilla_ThreadSafety_h */
