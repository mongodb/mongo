/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_SweepingAPI_h
#define js_SweepingAPI_h

#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"

#include "jstypes.h"

#include "js/GCAnnotations.h"
#include "js/GCPolicyAPI.h"
#include "js/RootingAPI.h"

namespace js {
namespace gc {

JS_PUBLIC_API void LockStoreBuffer(JSRuntime* runtime);
JS_PUBLIC_API void UnlockStoreBuffer(JSRuntime* runtim);

class AutoLockStoreBuffer {
  JSRuntime* runtime;

 public:
  explicit AutoLockStoreBuffer(JSRuntime* runtime) : runtime(runtime) {
    LockStoreBuffer(runtime);
  }
  ~AutoLockStoreBuffer() { UnlockStoreBuffer(runtime); }
};

}  // namespace gc
}  // namespace js

namespace JS {
namespace detail {
class WeakCacheBase;
}  // namespace detail

namespace shadow {
JS_PUBLIC_API void RegisterWeakCache(JS::Zone* zone,
                                     JS::detail::WeakCacheBase* cachep);
JS_PUBLIC_API void RegisterWeakCache(JSRuntime* rt,
                                     JS::detail::WeakCacheBase* cachep);
}  // namespace shadow

namespace detail {

class WeakCacheBase : public mozilla::LinkedListElement<WeakCacheBase> {
  WeakCacheBase() = delete;
  explicit WeakCacheBase(const WeakCacheBase&) = delete;

 public:
  enum NeedsLock : bool { LockStoreBuffer = true, DontLockStoreBuffer = false };

  explicit WeakCacheBase(JS::Zone* zone) {
    shadow::RegisterWeakCache(zone, this);
  }
  explicit WeakCacheBase(JSRuntime* rt) { shadow::RegisterWeakCache(rt, this); }
  WeakCacheBase(WeakCacheBase&& other) = default;
  virtual ~WeakCacheBase() = default;

  virtual size_t traceWeak(JSTracer* trc, NeedsLock needLock) = 0;

  // Sweeping will be skipped if the cache is empty already.
  virtual bool empty() = 0;

  // Enable/disable read barrier during incremental sweeping and set the tracer
  // to use.
  virtual bool setIncrementalBarrierTracer(JSTracer* trc) {
    // Derived classes do not support incremental barriers by default.
    return false;
  }
  virtual bool needsIncrementalBarrier() const {
    // Derived classes do not support incremental barriers by default.
    return false;
  }
};

}  // namespace detail

// A WeakCache stores the given Sweepable container and links itself into a
// list of such caches that are swept during each GC. A WeakCache can be
// specific to a zone, or across a whole runtime, depending on which
// constructor is used.
template <typename T>
class WeakCache : protected detail::WeakCacheBase,
                  public js::MutableWrappedPtrOperations<T, WeakCache<T>> {
  T cache;

 public:
  using Type = T;

  template <typename... Args>
  explicit WeakCache(Zone* zone, Args&&... args)
      : WeakCacheBase(zone), cache(std::forward<Args>(args)...) {}
  template <typename... Args>
  explicit WeakCache(JSRuntime* rt, Args&&... args)
      : WeakCacheBase(rt), cache(std::forward<Args>(args)...) {}

  const T& get() const { return cache; }
  T& get() { return cache; }

  size_t traceWeak(JSTracer* trc, NeedsLock needsLock) override {
    // Take the store buffer lock in case sweeping triggers any generational
    // post barriers. This is not always required and WeakCache specializations
    // may delay or skip taking the lock as appropriate.
    mozilla::Maybe<js::gc::AutoLockStoreBuffer> lock;
    if (needsLock) {
      lock.emplace(trc->runtime());
    }

    GCPolicy<T>::traceWeak(trc, &cache);
    return 0;
  }

  bool empty() override { return cache.empty(); }
} JS_HAZ_NON_GC_POINTER;

}  // namespace JS

#endif  // js_SweepingAPI_h
