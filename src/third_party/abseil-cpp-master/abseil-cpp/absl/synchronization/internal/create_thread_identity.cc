// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>
#include <new>

// This file is a no-op if the required LowLevelAlloc support is missing.
#include "absl/base/internal/low_level_alloc.h"
#ifndef ABSL_LOW_LEVEL_ALLOC_MISSING

#include <string.h>

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/thread_identity.h"
#include "absl/synchronization/internal/per_thread_sem.h"

namespace absl {
namespace synchronization_internal {

// ThreadIdentity storage is persistent, we maintain a free-list of previously
// released ThreadIdentity objects.
static base_internal::SpinLock freelist_lock(base_internal::kLinkerInitialized);
static base_internal::ThreadIdentity* thread_identity_freelist;

// A per-thread destructor for reclaiming associated ThreadIdentity objects.
// Since we must preserve their storage we cache them for re-use.
static void ReclaimThreadIdentity(void* v) {
  base_internal::ThreadIdentity* identity =
      static_cast<base_internal::ThreadIdentity*>(v);

  // all_locks might have been allocated by the Mutex implementation.
  // We free it here when we are notified that our thread is dying.
  if (identity->per_thread_synch.all_locks != nullptr) {
    base_internal::LowLevelAlloc::Free(identity->per_thread_synch.all_locks);
  }

  // We must explicitly clear the current thread's identity:
  // (a) Subsequent (unrelated) per-thread destructors may require an identity.
  //     We must guarantee a new identity is used in this case (this instructor
  //     will be reinvoked up to PTHREAD_DESTRUCTOR_ITERATIONS in this case).
  // (b) ThreadIdentity implementations may depend on memory that is not
  //     reinitialized before reuse.  We must allow explicit clearing of the
  //     association state in this case.
  base_internal::ClearCurrentThreadIdentity();
  {
    base_internal::SpinLockHolder l(&freelist_lock);
    identity->next = thread_identity_freelist;
    thread_identity_freelist = identity;
  }
}

// Return value rounded up to next multiple of align.
// Align must be a power of two.
static intptr_t RoundUp(intptr_t addr, intptr_t align) {
  return (addr + align - 1) & ~(align - 1);
}

static base_internal::ThreadIdentity* NewThreadIdentity() {
  base_internal::ThreadIdentity* identity = nullptr;

  {
    // Re-use a previously released object if possible.
    base_internal::SpinLockHolder l(&freelist_lock);
    if (thread_identity_freelist) {
      identity = thread_identity_freelist;  // Take list-head.
      thread_identity_freelist = thread_identity_freelist->next;
    }
  }

  if (identity == nullptr) {
    // Allocate enough space to align ThreadIdentity to a multiple of
    // PerThreadSynch::kAlignment. This space is never released (it is
    // added to a freelist by ReclaimThreadIdentity instead).
    void* allocation = base_internal::LowLevelAlloc::Alloc(
        sizeof(*identity) + base_internal::PerThreadSynch::kAlignment - 1);
    // Round up the address to the required alignment.
    identity = reinterpret_cast<base_internal::ThreadIdentity*>(
        RoundUp(reinterpret_cast<intptr_t>(allocation),
                base_internal::PerThreadSynch::kAlignment));
  }
  memset(identity, 0, sizeof(*identity));

  return identity;
}

// Allocates and attaches ThreadIdentity object for the calling thread.  Returns
// the new identity.
// REQUIRES: CurrentThreadIdentity(false) == nullptr
base_internal::ThreadIdentity* CreateThreadIdentity() {
  base_internal::ThreadIdentity* identity = NewThreadIdentity();
  PerThreadSem::Init(identity);
  // Associate the value with the current thread, and attach our destructor.
  base_internal::SetCurrentThreadIdentity(identity, ReclaimThreadIdentity);
  return identity;
}

}  // namespace synchronization_internal
}  // namespace absl

#endif  // ABSL_LOW_LEVEL_ALLOC_MISSING
