/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCContext_h
#define gc_GCContext_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/ThreadLocal.h"

#include "jspubtd.h"
#include "jstypes.h"                  // JS_PUBLIC_API
#include "gc/GCEnum.h"                // js::MemoryUse
#include "jit/ExecutableAllocator.h"  // jit::JitPoisonRangeVector
#include "js/Utility.h"               // js_free

struct JS_PUBLIC_API JSRuntime;

namespace js {

class AutoTouchingGrayThings;

namespace gc {

class AutoSetThreadGCUse;
class AutoSetThreadIsSweeping;

enum class GCUse {
  // This thread is not running in the garbage collector.
  None,

  // This thread is currently collecting. Used when no finer detail is known.
  Unspecified,

  // This thread is currently marking GC things. This thread could be the main
  // thread or a helper thread doing sweep-marking.
  Marking,

  // This thread is currently sweeping GC things. This thread could be the
  // main thread or a helper thread while the main thread is running the
  // mutator.
  Sweeping,

  // Whether this thread is currently finalizing GC things. This thread could
  // be the main thread or a helper thread doing finalization while the main
  // thread is running the mutator.
  Finalizing
};

}  // namespace gc
}  // namespace js

namespace JS {

/*
 * GCContext is by GC operations that can run on or off the main thread.
 *
 * Its main function is to provide methods to free memory and update memory
 * accounting. For convenience, it also has delete_ convenience methods that
 * also call destructors.
 *
 * It is passed to finalizers and other sweep-phase hooks as JSContext is not
 * available off the main thread.
 */
class GCContext {
  using Cell = js::gc::Cell;
  using MemoryUse = js::MemoryUse;

  JSRuntime* const runtime_;

  js::jit::JitPoisonRangeVector jitPoisonRanges;

  // Which part of the garbage collector this context is running at the moment.
  js::gc::GCUse gcUse_ = js::gc::GCUse::None;
  friend class js::gc::AutoSetThreadGCUse;
  friend class js::gc::AutoSetThreadIsSweeping;

#ifdef DEBUG
  // The specific zone currently being swept, if any.
  Zone* gcSweepZone_ = nullptr;

  // Whether this thread is currently manipulating possibly-gray GC things.
  size_t isTouchingGrayThings_ = false;
  friend class js::AutoTouchingGrayThings;
#endif

 public:
  explicit GCContext(JSRuntime* maybeRuntime);
  ~GCContext();

  JSRuntime* runtime() const {
    MOZ_ASSERT(onMainThread());
    return runtimeFromAnyThread();
  }
  JSRuntime* runtimeFromAnyThread() const {
    MOZ_ASSERT(runtime_);
    return runtime_;
  }

  js::gc::GCUse gcUse() const { return gcUse_; }
  bool isCollecting() const { return gcUse() != js::gc::GCUse::None; }
  bool isFinalizing() const { return gcUse_ == js::gc::GCUse::Finalizing; }

#ifdef DEBUG
  bool onMainThread() const {
    return js::CurrentThreadCanAccessRuntime(runtime_);
  }

  Zone* gcSweepZone() const { return gcSweepZone_; }
  bool isTouchingGrayThings() const { return isTouchingGrayThings_; }
#endif

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
    return jitPoisonRanges.append(range);
  }
  bool hasJitCodeToPoison() const { return !jitPoisonRanges.empty(); }
  void poisonJitCode();

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

}  // namespace JS

namespace js {

/* Thread Local Storage for storing the GCContext for a thread. */
extern MOZ_THREAD_LOCAL(JS::GCContext*) TlsGCContext;

inline JS::GCContext* MaybeGetGCContext() {
  if (!TlsGCContext.init()) {
    return nullptr;
  }
  return TlsGCContext.get();
}

class MOZ_RAII AutoTouchingGrayThings {
 public:
#ifdef DEBUG
  AutoTouchingGrayThings() { TlsGCContext.get()->isTouchingGrayThings_++; }
  ~AutoTouchingGrayThings() {
    JS::GCContext* gcx = TlsGCContext.get();
    MOZ_ASSERT(gcx->isTouchingGrayThings_);
    gcx->isTouchingGrayThings_--;
  }
#else
  AutoTouchingGrayThings() {}
#endif
};

#ifdef DEBUG

inline bool CurrentThreadIsGCMarking() {
  JS::GCContext* gcx = MaybeGetGCContext();
  return gcx && gcx->gcUse() == gc::GCUse::Marking;
}

inline bool CurrentThreadIsGCSweeping() {
  JS::GCContext* gcx = MaybeGetGCContext();
  return gcx && gcx->gcUse() == gc::GCUse::Sweeping;
}

inline bool CurrentThreadIsGCFinalizing() {
  JS::GCContext* gcx = MaybeGetGCContext();
  return gcx && gcx->gcUse() == gc::GCUse::Finalizing;
}

inline bool CurrentThreadIsTouchingGrayThings() {
  JS::GCContext* gcx = MaybeGetGCContext();
  return gcx && gcx->isTouchingGrayThings();
}

inline bool CurrentThreadIsPerformingGC() {
  JS::GCContext* gcx = MaybeGetGCContext();
  return gcx && gcx->isCollecting();
}

#endif

}  // namespace js

#endif  // gc_GCContext_h
