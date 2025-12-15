/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCompileQueue_h
#define jit_BaselineCompileQueue_h

#include "gc/Barrier.h"
#include "jit/JitOptions.h"

class JSScript;

namespace js {
namespace jit {

class BaselineCompileQueue {
 public:
  static constexpr uint32_t MaxCapacity = 64;

 private:
  uint32_t numQueued_ = 0;
  HeapPtr<JSScript*> queue_[MaxCapacity];

 public:
  uint32_t numQueued() const { return numQueued_; }

  static constexpr size_t offsetOfQueue() {
    return offsetof(BaselineCompileQueue, queue_);
  }
  static constexpr size_t offsetOfNumQueued() {
    return offsetof(BaselineCompileQueue, numQueued_);
  }

  JSScript* pop() {
    // To keep our invariants simple, we pop from the end of the queue.
    MOZ_ASSERT(!isEmpty());
    assertInvariants();
    numQueued_--;
    JSScript* result = queue_[numQueued_];
    queue_[numQueued_] = nullptr;
    assertInvariants();
    return result;
  }

  bool isEmpty() const { return numQueued_ == 0; }

  MOZ_ALWAYS_INLINE
  void assertInvariants() const {
    // The queue always contains |numQueued| JSScript* pointers,
    // followed by |Capacity - numQueued| null pointers.
#ifdef DEBUG
    MOZ_ASSERT(numQueued_ <= JitOptions.baselineQueueCapacity);
    MOZ_ASSERT(JitOptions.baselineQueueCapacity <= MaxCapacity);
    for (uint32_t i = 0; i < numQueued_; i++) {
      MOZ_ASSERT(queue_[i]);
    }
    for (uint32_t i = numQueued_; i < MaxCapacity; i++) {
      MOZ_ASSERT(!queue_[i]);
    }
#endif
  }
  void trace(JSTracer* trc);
  void remove(JSScript* script);
};

}  // namespace jit
}  // namespace js

#endif  // jit_BaselineCompileQueue_h
