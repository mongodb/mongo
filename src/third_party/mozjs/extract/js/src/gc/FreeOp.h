/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_FreeOp_h
#define gc_FreeOp_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jstypes.h"                  // JS_PUBLIC_API
#include "gc/GCEnum.h"                // js::MemoryUse
#include "jit/ExecutableAllocator.h"  // jit::JitPoisonRangeVector
#include "js/AllocPolicy.h"           // SystemAllocPolicy
#include "js/MemoryFunctions.h"       // JSFreeOp
#include "js/Utility.h"               // AutoEnterOOMUnsafeRegion, js_free
#include "js/Vector.h"                // js::Vector

struct JS_PUBLIC_API JSRuntime;

namespace js {
namespace gc {
class AutoSetThreadIsPerformingGC;
}  // namespace gc
}  // namespace js

/*
 * A JSFreeOp can do one thing: free memory. For convenience, it has delete_
 * convenience methods that also call destructors.
 *
 * JSFreeOp is passed to finalizers and other sweep-phase hooks so that we do
 * not need to pass a JSContext to those hooks.
 */
class JSFreeOp {
  using Cell = js::gc::Cell;
  using MemoryUse = js::MemoryUse;

  JSRuntime* runtime_;

  js::jit::JitPoisonRangeVector jitPoisonRanges;

  const bool isDefault;
  bool isCollecting_;

  friend class js::gc::AutoSetThreadIsPerformingGC;

 public:
  explicit JSFreeOp(JSRuntime* maybeRuntime, bool isDefault = false);
  ~JSFreeOp();

  JSRuntime* runtime() const {
    MOZ_ASSERT(runtime_);
    return runtime_;
  }

  bool onMainThread() const { return runtime_ != nullptr; }

  bool maybeOnHelperThread() const {
    // Sometimes background finalization happens on the main thread so
    // runtime_ being null doesn't always mean we are off thread.
    return !runtime_;
  }

  bool isDefaultFreeOp() const { return isDefault; }
  bool isCollecting() const { return isCollecting_; }

  // Deprecated. Where possible, memory should be tracked against the owning GC
  // thing by calling js::AddCellMemory and the memory freed with free_() below.
  void freeUntracked(void* p) { js_free(p); }

  // Free memory associated with a GC thing and update the memory accounting.
  //
  // The memory should have been associated with the GC thing using
  // js::InitReservedSlot or js::InitObjectPrivate, or possibly
  // js::AddCellMemory.
  void free_(Cell* cell, void* p, size_t nbytes, MemoryUse use);

  bool appendJitPoisonRange(const js::jit::JitPoisonRange& range) {
    // JSFreeOps other than the defaultFreeOp() are constructed on the stack,
    // and won't hold onto the pointers to free indefinitely.
    MOZ_ASSERT(!isDefaultFreeOp());

    return jitPoisonRanges.append(range);
  }

  // Deprecated. Where possible, memory should be tracked against the owning GC
  // thing by calling js::AddCellMemory and the memory freed with delete_()
  // below.
  template <class T>
  void deleteUntracked(T* p) {
    if (p) {
      p->~T();
      js_free(p);
    }
  }

  // Delete a C++ object that was associated with a GC thing and update the
  // memory accounting. The size is determined by the type T.
  //
  // The memory should have been associated with the GC thing using
  // js::InitReservedSlot or js::InitObjectPrivate, or possibly
  // js::AddCellMemory.
  template <class T>
  void delete_(Cell* cell, T* p, MemoryUse use) {
    delete_(cell, p, sizeof(T), use);
  }

  // Delete a C++ object that was associated with a GC thing and update the
  // memory accounting.
  //
  // The memory should have been associated with the GC thing using
  // js::InitReservedSlot or js::InitObjectPrivate, or possibly
  // js::AddCellMemory.
  template <class T>
  void delete_(Cell* cell, T* p, size_t nbytes, MemoryUse use) {
    if (p) {
      p->~T();
      free_(cell, p, nbytes, use);
    }
  }

  // Release a RefCounted object that was associated with a GC thing and update
  // the memory accounting.
  //
  // The memory should have been associated with the GC thing using
  // js::InitReservedSlot or js::InitObjectPrivate, or possibly
  // js::AddCellMemory.
  //
  // This counts the memory once per association with a GC thing. It's not
  // expected that the same object is associated with more than one GC thing in
  // each zone. If this is the case then some other form of accounting would be
  // more appropriate.
  template <class T>
  void release(Cell* cell, T* p, MemoryUse use) {
    release(cell, p, sizeof(T), use);
  }

  // Release a RefCounted object and that was associated with a GC thing and
  // update the memory accounting.
  //
  // The memory should have been associated with the GC thing using
  // js::InitReservedSlot or js::InitObjectPrivate, or possibly
  // js::AddCellMemory.
  template <class T>
  void release(Cell* cell, T* p, size_t nbytes, MemoryUse use);

  // Update the memory accounting for a GC for memory freed by some other
  // method.
  void removeCellMemory(Cell* cell, size_t nbytes, MemoryUse use);
};

#endif  // gc_FreeOp_h
