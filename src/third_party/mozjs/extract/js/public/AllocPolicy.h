/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS allocation policies.
 *
 * The allocators here are for system memory with lifetimes which are not
 * managed by the GC. See the comment at the top of vm/MallocProvider.h.
 */

#ifndef js_AllocPolicy_h
#define js_AllocPolicy_h

#include "js/TypeDecls.h"
#include "js/Utility.h"

extern MOZ_COLD JS_PUBLIC_API void JS_ReportOutOfMemory(JSContext* cx);

namespace js {

class FrontendContext;

enum class AllocFunction { Malloc, Calloc, Realloc };

/* Base class allocation policies providing allocation methods. */
class AllocPolicyBase {
 public:
  template <typename T>
  T* maybe_pod_arena_malloc(arena_id_t arenaId, size_t numElems) {
    return js_pod_arena_malloc<T>(arenaId, numElems);
  }
  template <typename T>
  T* maybe_pod_arena_calloc(arena_id_t arenaId, size_t numElems) {
    return js_pod_arena_calloc<T>(arenaId, numElems);
  }
  template <typename T>
  T* maybe_pod_arena_realloc(arena_id_t arenaId, T* p, size_t oldSize,
                             size_t newSize) {
    return js_pod_arena_realloc<T>(arenaId, p, oldSize, newSize);
  }
  template <typename T>
  T* pod_arena_malloc(arena_id_t arenaId, size_t numElems) {
    return maybe_pod_arena_malloc<T>(arenaId, numElems);
  }
  template <typename T>
  T* pod_arena_calloc(arena_id_t arenaId, size_t numElems) {
    return maybe_pod_arena_calloc<T>(arenaId, numElems);
  }
  template <typename T>
  T* pod_arena_realloc(arena_id_t arenaId, T* p, size_t oldSize,
                       size_t newSize) {
    return maybe_pod_arena_realloc<T>(arenaId, p, oldSize, newSize);
  }

  template <typename T>
  T* maybe_pod_malloc(size_t numElems) {
    return maybe_pod_arena_malloc<T>(js::MallocArena, numElems);
  }
  template <typename T>
  T* maybe_pod_calloc(size_t numElems) {
    return maybe_pod_arena_calloc<T>(js::MallocArena, numElems);
  }
  template <typename T>
  T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
    return maybe_pod_arena_realloc<T>(js::MallocArena, p, oldSize, newSize);
  }
  template <typename T>
  T* pod_malloc(size_t numElems) {
    return pod_arena_malloc<T>(js::MallocArena, numElems);
  }
  template <typename T>
  T* pod_calloc(size_t numElems) {
    return pod_arena_calloc<T>(js::MallocArena, numElems);
  }
  template <typename T>
  T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
    return pod_arena_realloc<T>(js::MallocArena, p, oldSize, newSize);
  }

  template <typename T>
  void free_(T* p, size_t numElems = 0) {
    js_free(p);
  }
};

/* Policy for using system memory functions and doing no error reporting. */
class SystemAllocPolicy : public AllocPolicyBase {
 public:
  void reportAllocOverflow() const {}
  bool checkSimulatedOOM() const { return !js::oom::ShouldFailWithOOM(); }
};

MOZ_COLD JS_PUBLIC_API void ReportOutOfMemory(JSContext* cx);
MOZ_COLD JS_PUBLIC_API void ReportOutOfMemory(FrontendContext* fc);

// An out of memory condition which is easily user generatable and should
// be specially handled to try and avoid a tab crash.
MOZ_COLD JS_PUBLIC_API void ReportLargeOutOfMemory(JSContext* cx);

/*
 * Allocation policy that calls the system memory functions and reports errors
 * to the context. Since the JSContext given on construction is stored for
 * the lifetime of the container, this policy may only be used for containers
 * whose lifetime is a shorter than the given JSContext.
 *
 * FIXME bug 647103 - rewrite this in terms of temporary allocation functions,
 * not the system ones.
 */
class JS_PUBLIC_API TempAllocPolicy : public AllocPolicyBase {
  // Type tag for context_bits_
  static constexpr uintptr_t JsContextTag = 0x1;

  // Either a JSContext* (if JsContextTag is set), or FrontendContext*
  uintptr_t const context_bits_;

  MOZ_ALWAYS_INLINE bool hasJSContext() const {
    return (context_bits_ & JsContextTag) == JsContextTag;
  }

  MOZ_ALWAYS_INLINE JSContext* cx() const {
    MOZ_ASSERT(hasJSContext());
    return reinterpret_cast<JSContext*>(context_bits_ ^ JsContextTag);
  }

  MOZ_ALWAYS_INLINE FrontendContext* fc() const {
    MOZ_ASSERT(!hasJSContext());
    return reinterpret_cast<FrontendContext*>(context_bits_);
  }

  /*
   * Non-inline helper to call JSRuntime::onOutOfMemory with minimal
   * code bloat.
   */
  void* onOutOfMemory(arena_id_t arenaId, AllocFunction allocFunc,
                      size_t nbytes, void* reallocPtr = nullptr);

  template <typename T>
  T* onOutOfMemoryTyped(arena_id_t arenaId, AllocFunction allocFunc,
                        size_t numElems, void* reallocPtr = nullptr) {
    size_t bytes;
    if (MOZ_UNLIKELY(!CalculateAllocSize<T>(numElems, &bytes))) {
      return nullptr;
    }
    return static_cast<T*>(
        onOutOfMemory(arenaId, allocFunc, bytes, reallocPtr));
  }

 public:
  MOZ_IMPLICIT TempAllocPolicy(JSContext* cx)
      : context_bits_(uintptr_t(cx) | JsContextTag) {
    MOZ_ASSERT((uintptr_t(cx) & JsContextTag) == 0);
  }
  MOZ_IMPLICIT TempAllocPolicy(FrontendContext* fc)
      : context_bits_(uintptr_t(fc)) {
    MOZ_ASSERT((uintptr_t(fc) & JsContextTag) == 0);
  }

  template <typename T>
  T* pod_arena_malloc(arena_id_t arenaId, size_t numElems) {
    T* p = this->maybe_pod_arena_malloc<T>(arenaId, numElems);
    if (MOZ_UNLIKELY(!p)) {
      p = onOutOfMemoryTyped<T>(arenaId, AllocFunction::Malloc, numElems);
    }
    return p;
  }

  template <typename T>
  T* pod_arena_calloc(arena_id_t arenaId, size_t numElems) {
    T* p = this->maybe_pod_arena_calloc<T>(arenaId, numElems);
    if (MOZ_UNLIKELY(!p)) {
      p = onOutOfMemoryTyped<T>(arenaId, AllocFunction::Calloc, numElems);
    }
    return p;
  }

  template <typename T>
  T* pod_arena_realloc(arena_id_t arenaId, T* prior, size_t oldSize,
                       size_t newSize) {
    T* p2 = this->maybe_pod_arena_realloc<T>(arenaId, prior, oldSize, newSize);
    if (MOZ_UNLIKELY(!p2)) {
      p2 = onOutOfMemoryTyped<T>(arenaId, AllocFunction::Realloc, newSize,
                                 prior);
    }
    return p2;
  }

  template <typename T>
  T* pod_malloc(size_t numElems) {
    return pod_arena_malloc<T>(js::MallocArena, numElems);
  }

  template <typename T>
  T* pod_calloc(size_t numElems) {
    return pod_arena_calloc<T>(js::MallocArena, numElems);
  }

  template <typename T>
  T* pod_realloc(T* prior, size_t oldSize, size_t newSize) {
    return pod_arena_realloc<T>(js::MallocArena, prior, oldSize, newSize);
  }

  template <typename T>
  void free_(T* p, size_t numElems = 0) {
    js_free(p);
  }

  void reportAllocOverflow() const;

  bool checkSimulatedOOM() const {
    if (js::oom::ShouldFailWithOOM()) {
      if (hasJSContext()) {
        ReportOutOfMemory(cx());
      } else {
        ReportOutOfMemory(fc());
      }
      return false;
    }

    return true;
  }
};

/*
 * A replacement for MallocAllocPolicy that allocates in the JS heap and adds no
 * extra behaviours.
 *
 * This is currently used for allocating source buffers for parsing. Since these
 * are temporary and will not be freed by GC, the memory is not tracked by the
 * usual accounting.
 */
class MallocAllocPolicy : public AllocPolicyBase {
 public:
  void reportAllocOverflow() const {}

  [[nodiscard]] bool checkSimulatedOOM() const { return true; }
};

} /* namespace js */

#endif /* js_AllocPolicy_h */
