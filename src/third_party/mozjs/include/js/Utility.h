/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Utility_h
#define js_Utility_h

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/Compiler.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/UniquePtr.h"

#include <stdlib.h>
#include <string.h>
#include <type_traits>
#include <utility>

#include "jstypes.h"
#include "mozmemory.h"
#include "js/TypeDecls.h"

/* The public JS engine namespace. */
namespace JS {}

/* The mozilla-shared reusable template/utility namespace. */
namespace mozilla {}

/* The private JS engine namespace. */
namespace js {}

extern MOZ_NORETURN MOZ_COLD JS_PUBLIC_API void JS_Assert(const char* s,
                                                          const char* file,
                                                          int ln);

/*
 * Custom allocator support for SpiderMonkey
 */
#if defined JS_USE_CUSTOM_ALLOCATOR
#  include "jscustomallocator.h"
#else

namespace js {

/*
 * Thread types are used to tag threads for certain kinds of testing (see
 * below), and also used to characterize threads in the thread scheduler (see
 * js/src/vm/HelperThreads.cpp).
 *
 * Please update oom::FirstThreadTypeToTest and oom::LastThreadTypeToTest when
 * adding new thread types.
 */
enum ThreadType {
  THREAD_TYPE_NONE = 0,              // 0
  THREAD_TYPE_MAIN,                  // 1
  THREAD_TYPE_WASM_COMPILE_TIER1,    // 2
  THREAD_TYPE_WASM_COMPILE_TIER2,    // 3
  THREAD_TYPE_ION,                   // 4
  THREAD_TYPE_COMPRESS,              // 5
  THREAD_TYPE_GCPARALLEL,            // 6
  THREAD_TYPE_PROMISE_TASK,          // 7
  THREAD_TYPE_ION_FREE,              // 8
  THREAD_TYPE_WASM_GENERATOR_TIER2,  // 9
  THREAD_TYPE_WORKER,                // 10
  THREAD_TYPE_DELAZIFY,              // 11
  THREAD_TYPE_DELAZIFY_FREE,         // 12
  THREAD_TYPE_MAX                    // Used to check shell function arguments
};

namespace oom {

/*
 * Theads are tagged only in certain debug contexts.  Notably, to make testing
 * OOM in certain helper threads more effective, we allow restricting the OOM
 * testing to a certain helper thread type. This allows us to fail e.g. in
 * off-thread script parsing without causing an OOM in the active thread first.
 *
 * Getter/Setter functions to encapsulate mozilla::ThreadLocal, implementation
 * is in util/Utility.cpp.
 */
#  if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

// Define the range of threads tested by simulated OOM testing and the
// like. Testing worker threads is not supported.
const ThreadType FirstThreadTypeToTest = THREAD_TYPE_MAIN;
const ThreadType LastThreadTypeToTest = THREAD_TYPE_WASM_GENERATOR_TIER2;

extern bool InitThreadType(void);
extern void SetThreadType(ThreadType);
extern JS_PUBLIC_API uint32_t GetThreadType(void);

#  else

inline bool InitThreadType(void) { return true; }
inline void SetThreadType(ThreadType t){};
inline uint32_t GetThreadType(void) { return 0; }
inline uint32_t GetAllocationThreadType(void) { return 0; }
inline uint32_t GetStackCheckThreadType(void) { return 0; }
inline uint32_t GetInterruptCheckThreadType(void) { return 0; }

#  endif

} /* namespace oom */
} /* namespace js */

#  if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

#    ifdef JS_OOM_BREAKPOINT
#      if defined(_MSC_VER)
static MOZ_NEVER_INLINE void js_failedAllocBreakpoint() {
  __asm {}
  ;
}
#      else
static MOZ_NEVER_INLINE void js_failedAllocBreakpoint() { asm(""); }
#      endif
#      define JS_OOM_CALL_BP_FUNC() js_failedAllocBreakpoint()
#    else
#      define JS_OOM_CALL_BP_FUNC() \
        do {                        \
        } while (0)
#    endif

namespace js {
namespace oom {

/*
 * Out of memory testing support.  We provide various testing functions to
 * simulate OOM conditions and so we can test that they are handled correctly.
 */
class FailureSimulator {
 public:
  enum class Kind : uint8_t { Nothing, OOM, StackOOM, Interrupt };

 private:
  Kind kind_ = Kind::Nothing;
  uint32_t targetThread_ = 0;
  uint64_t maxChecks_ = UINT64_MAX;
  uint64_t counter_ = 0;
  bool failAlways_ = true;
  bool inUnsafeRegion_ = false;

 public:
  uint64_t maxChecks() const { return maxChecks_; }
  uint64_t counter() const { return counter_; }
  void setInUnsafeRegion(bool b) {
    MOZ_ASSERT(inUnsafeRegion_ != b);
    inUnsafeRegion_ = b;
  }
  uint32_t targetThread() const { return targetThread_; }
  bool isThreadSimulatingAny() const {
    return targetThread_ && targetThread_ == js::oom::GetThreadType() &&
           !inUnsafeRegion_;
  }
  bool isThreadSimulating(Kind kind) const {
    return kind_ == kind && isThreadSimulatingAny();
  }
  bool isSimulatedFailure(Kind kind) const {
    if (!isThreadSimulating(kind)) {
      return false;
    }
    return counter_ == maxChecks_ || (counter_ > maxChecks_ && failAlways_);
  }
  bool hadFailure(Kind kind) const {
    return kind_ == kind && counter_ >= maxChecks_;
  }
  bool shouldFail(Kind kind) {
    if (!isThreadSimulating(kind)) {
      return false;
    }
    counter_++;
    if (isSimulatedFailure(kind)) {
      JS_OOM_CALL_BP_FUNC();
      return true;
    }
    return false;
  }

  void simulateFailureAfter(Kind kind, uint64_t checks, uint32_t thread,
                            bool always);
  void reset();
};
extern JS_PUBLIC_DATA FailureSimulator simulator;

inline bool IsSimulatedOOMAllocation() {
  return simulator.isSimulatedFailure(FailureSimulator::Kind::OOM);
}

inline bool ShouldFailWithOOM() {
  return simulator.shouldFail(FailureSimulator::Kind::OOM);
}

inline bool HadSimulatedOOM() {
  return simulator.hadFailure(FailureSimulator::Kind::OOM);
}

/*
 * Out of stack space testing support, similar to OOM testing functions.
 */

inline bool IsSimulatedStackOOMCheck() {
  return simulator.isSimulatedFailure(FailureSimulator::Kind::StackOOM);
}

inline bool ShouldFailWithStackOOM() {
  return simulator.shouldFail(FailureSimulator::Kind::StackOOM);
}

inline bool HadSimulatedStackOOM() {
  return simulator.hadFailure(FailureSimulator::Kind::StackOOM);
}

/*
 * Interrupt testing support, similar to OOM testing functions.
 */

inline bool IsSimulatedInterruptCheck() {
  return simulator.isSimulatedFailure(FailureSimulator::Kind::Interrupt);
}

inline bool ShouldFailWithInterrupt() {
  return simulator.shouldFail(FailureSimulator::Kind::Interrupt);
}

inline bool HadSimulatedInterrupt() {
  return simulator.hadFailure(FailureSimulator::Kind::Interrupt);
}

} /* namespace oom */
} /* namespace js */

#    define JS_OOM_POSSIBLY_FAIL()                        \
      do {                                                \
        if (js::oom::ShouldFailWithOOM()) return nullptr; \
      } while (0)

#    define JS_OOM_POSSIBLY_FAIL_BOOL()                 \
      do {                                              \
        if (js::oom::ShouldFailWithOOM()) return false; \
      } while (0)

#    define JS_STACK_OOM_POSSIBLY_FAIL()                     \
      do {                                                   \
        if (js::oom::ShouldFailWithStackOOM()) return false; \
      } while (0)

#    define JS_INTERRUPT_POSSIBLY_FAIL()                             \
      do {                                                           \
        if (MOZ_UNLIKELY(js::oom::ShouldFailWithInterrupt())) {      \
          cx->requestInterrupt(js::InterruptReason::CallbackUrgent); \
          return cx->handleInterrupt();                              \
        }                                                            \
      } while (0)

#  else

#    define JS_OOM_POSSIBLY_FAIL() \
      do {                         \
      } while (0)
#    define JS_OOM_POSSIBLY_FAIL_BOOL() \
      do {                              \
      } while (0)
#    define JS_STACK_OOM_POSSIBLY_FAIL() \
      do {                               \
      } while (0)
#    define JS_INTERRUPT_POSSIBLY_FAIL() \
      do {                               \
      } while (0)
namespace js {
namespace oom {
static inline bool IsSimulatedOOMAllocation() { return false; }
static inline bool ShouldFailWithOOM() { return false; }
} /* namespace oom */
} /* namespace js */

#  endif /* DEBUG || JS_OOM_BREAKPOINT */

#  ifdef FUZZING
namespace js {
namespace oom {
extern JS_PUBLIC_DATA size_t largeAllocLimit;
extern void InitLargeAllocLimit();
} /* namespace oom */
} /* namespace js */

#    define JS_CHECK_LARGE_ALLOC(x)                                     \
      do {                                                              \
        if (js::oom::largeAllocLimit && x > js::oom::largeAllocLimit) { \
          if (getenv("MOZ_FUZZ_CRASH_ON_LARGE_ALLOC")) {                \
            MOZ_CRASH("Large allocation");                              \
          } else {                                                      \
            return nullptr;                                             \
          }                                                             \
        }                                                               \
      } while (0)
#  else
#    define JS_CHECK_LARGE_ALLOC(x) \
      do {                          \
      } while (0)
#  endif

namespace js {

/* Disable OOM testing in sections which are not OOM safe. */
struct MOZ_RAII JS_PUBLIC_DATA AutoEnterOOMUnsafeRegion {
  MOZ_NORETURN MOZ_COLD void crash(const char* reason) { crash_impl(reason); }
  MOZ_NORETURN MOZ_COLD void crash(size_t size, const char* reason) {
    crash_impl(reason);
  }

  using AnnotateOOMAllocationSizeCallback = void (*)(size_t);
  static mozilla::Atomic<AnnotateOOMAllocationSizeCallback, mozilla::Relaxed>
      annotateOOMSizeCallback;
  static void setAnnotateOOMAllocationSizeCallback(
      AnnotateOOMAllocationSizeCallback callback) {
    annotateOOMSizeCallback = callback;
  }

#  if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  AutoEnterOOMUnsafeRegion()
      : oomEnabled_(oom::simulator.isThreadSimulatingAny()) {
    if (oomEnabled_) {
      MOZ_ALWAYS_TRUE(owner_.compareExchange(nullptr, this));
      oom::simulator.setInUnsafeRegion(true);
    }
  }

  ~AutoEnterOOMUnsafeRegion() {
    if (oomEnabled_) {
      oom::simulator.setInUnsafeRegion(false);
      MOZ_ALWAYS_TRUE(owner_.compareExchange(this, nullptr));
    }
  }

 private:
  // Used to catch concurrent use from other threads.
  static mozilla::Atomic<AutoEnterOOMUnsafeRegion*> owner_;

  bool oomEnabled_;
#  endif
 private:
  static MOZ_NORETURN MOZ_COLD void crash_impl(const char* reason);
  static MOZ_NORETURN MOZ_COLD void crash_impl(size_t size, const char* reason);
};

} /* namespace js */

// Malloc allocation.

namespace js {

extern JS_PUBLIC_DATA arena_id_t MallocArena;
extern JS_PUBLIC_DATA arena_id_t ArrayBufferContentsArena;
extern JS_PUBLIC_DATA arena_id_t StringBufferArena;

extern void InitMallocAllocator();
extern void ShutDownMallocAllocator();

// This is a no-op if built without MOZ_MEMORY and MOZ_DEBUG.
extern void AssertJSStringBufferInCorrectArena(const void* ptr);

} /* namespace js */

static inline void* js_arena_malloc(arena_id_t arena, size_t bytes) {
  JS_OOM_POSSIBLY_FAIL();
  JS_CHECK_LARGE_ALLOC(bytes);
  return moz_arena_malloc(arena, bytes);
}

static inline void* js_malloc(size_t bytes) {
  return js_arena_malloc(js::MallocArena, bytes);
}

static inline void* js_arena_calloc(arena_id_t arena, size_t bytes) {
  JS_OOM_POSSIBLY_FAIL();
  JS_CHECK_LARGE_ALLOC(bytes);
  return moz_arena_calloc(arena, bytes, 1);
}

static inline void* js_arena_calloc(arena_id_t arena, size_t nmemb,
                                    size_t size) {
  JS_OOM_POSSIBLY_FAIL();
  JS_CHECK_LARGE_ALLOC(nmemb * size);
  return moz_arena_calloc(arena, nmemb, size);
}

static inline void* js_calloc(size_t bytes) {
  return js_arena_calloc(js::MallocArena, bytes);
}

static inline void* js_calloc(size_t nmemb, size_t size) {
  return js_arena_calloc(js::MallocArena, nmemb, size);
}

static inline void* js_arena_realloc(arena_id_t arena, void* p, size_t bytes) {
  // realloc() with zero size is not portable, as some implementations may
  // return nullptr on success and free |p| for this.  We assume nullptr
  // indicates failure and that |p| is still valid.
  MOZ_ASSERT(bytes != 0);

  JS_OOM_POSSIBLY_FAIL();
  JS_CHECK_LARGE_ALLOC(bytes);
  return moz_arena_realloc(arena, p, bytes);
}

static inline void* js_realloc(void* p, size_t bytes) {
  return js_arena_realloc(js::MallocArena, p, bytes);
}

static inline void js_free(void* p) {
  // Bug 1784164: This should call |moz_arena_free(js::MallocArena, p)| but we
  // currently can't enforce that all memory freed here was allocated by
  // js_malloc(). All other memory should go through a different allocator and
  // deallocator.
  free(p);
}
#endif /* JS_USE_CUSTOM_ALLOCATOR */

#include <new>

/*
 * [SMDOC] Low-level memory management in SpiderMonkey
 *
 *  ** Do not use the standard malloc/free/realloc: SpiderMonkey allows these
 *     to be redefined (via JS_USE_CUSTOM_ALLOCATOR) and Gecko even #define's
 *     these symbols.
 *
 *  ** Do not use the builtin C++ operator new and delete: these throw on
 *     error and we cannot override them not to.
 *
 * Allocation:
 *
 * - If the lifetime of the allocation is tied to the lifetime of a GC-thing
 *   (that is, finalizing the GC-thing will free the allocation), call one of
 *   the following functions:
 *
 *     JSContext::{pod_malloc,pod_calloc,pod_realloc}
 *     Zone::{pod_malloc,pod_calloc,pod_realloc}
 *
 *   These functions accumulate the number of bytes allocated which is used as
 *   part of the GC-triggering heuristics.
 *
 *   The difference between the JSContext and Zone versions is that the
 *   cx version report an out-of-memory error on OOM. (This follows from the
 *   general SpiderMonkey idiom that a JSContext-taking function reports its
 *   own errors.)
 *
 *   If you don't want to report an error on failure, there are maybe_ versions
 *   of these methods available too, e.g. maybe_pod_malloc.
 *
 *   The methods above use templates to allow allocating memory suitable for an
 *   array of a given type and number of elements. There are _with_extra
 *   versions to allow allocating an area of memory which is larger by a
 *   specified number of bytes, e.g. pod_malloc_with_extra.
 *
 *   These methods are available on a JSRuntime, but calling them is
 *   discouraged. Memory attributed to a runtime can only be reclaimed by full
 *   GCs, and we try to avoid those where possible.
 *
 * - Otherwise, use js_malloc/js_realloc/js_calloc/js_new
 *
 * Deallocation:
 *
 * - Ordinarily, use js_free/js_delete.
 *
 * - For deallocations during GC finalization, use one of the following
 *   operations on the JS::GCContext provided to the finalizer:
 *
 *     JS::GCContext::{free_,delete_}
 */

/*
 * Given a class which should provide a 'new' method, add
 * JS_DECLARE_NEW_METHODS (see js::MallocProvider for an example).
 *
 * Note: Do not add a ; at the end of a use of JS_DECLARE_NEW_METHODS,
 * or the build will break.
 */
#define JS_DECLARE_NEW_METHODS(NEWNAME, ALLOCATOR, QUALIFIERS)              \
  template <class T, typename... Args>                                      \
  QUALIFIERS T* MOZ_HEAP_ALLOCATOR NEWNAME(Args&&... args) {                \
    static_assert(                                                          \
        alignof(T) <= alignof(max_align_t),                                 \
        "over-aligned type is not supported by JS_DECLARE_NEW_METHODS");    \
    void* memory = ALLOCATOR(sizeof(T));                                    \
    return MOZ_LIKELY(memory) ? new (memory) T(std::forward<Args>(args)...) \
                              : nullptr;                                    \
  }

/*
 * Given a class which should provide a 'new' method that takes an arena as
 * its first argument, add JS_DECLARE_NEW_ARENA_METHODS
 * (see js::MallocProvider for an example).
 *
 * Note: Do not add a ; at the end of a use of JS_DECLARE_NEW_ARENA_METHODS,
 * or the build will break.
 */
#define JS_DECLARE_NEW_ARENA_METHODS(NEWNAME, ALLOCATOR, QUALIFIERS)           \
  template <class T, typename... Args>                                         \
  QUALIFIERS T* MOZ_HEAP_ALLOCATOR NEWNAME(arena_id_t arena, Args&&... args) { \
    static_assert(                                                             \
        alignof(T) <= alignof(max_align_t),                                    \
        "over-aligned type is not supported by JS_DECLARE_NEW_ARENA_METHODS"); \
    void* memory = ALLOCATOR(arena, sizeof(T));                                \
    return MOZ_LIKELY(memory) ? new (memory) T(std::forward<Args>(args)...)    \
                              : nullptr;                                       \
  }

/*
 * Given a class which should provide 'make' methods, add
 * JS_DECLARE_MAKE_METHODS (see js::MallocProvider for an example).  This
 * method is functionally the same as JS_DECLARE_NEW_METHODS: it just declares
 * methods that return mozilla::UniquePtr instances that will singly-manage
 * ownership of the created object.
 *
 * Note: Do not add a ; at the end of a use of JS_DECLARE_MAKE_METHODS,
 * or the build will break.
 */
#define JS_DECLARE_MAKE_METHODS(MAKENAME, NEWNAME, QUALIFIERS)             \
  template <class T, typename... Args>                                     \
  QUALIFIERS mozilla::UniquePtr<T, JS::DeletePolicy<T>> MOZ_HEAP_ALLOCATOR \
  MAKENAME(Args&&... args) {                                               \
    T* ptr = NEWNAME<T>(std::forward<Args>(args)...);                      \
    return mozilla::UniquePtr<T, JS::DeletePolicy<T>>(ptr);                \
  }

JS_DECLARE_NEW_METHODS(js_new, js_malloc, static MOZ_ALWAYS_INLINE)
JS_DECLARE_NEW_ARENA_METHODS(js_arena_new, js_arena_malloc,
                             static MOZ_ALWAYS_INLINE)

namespace js {

/*
 * Calculate the number of bytes needed to allocate |numElems| contiguous
 * instances of type |T|.  Return false if the calculation overflowed.
 */
template <typename T>
[[nodiscard]] inline bool CalculateAllocSize(size_t numElems,
                                             size_t* bytesOut) {
  *bytesOut = numElems * sizeof(T);
  return (numElems & mozilla::tl::MulOverflowMask<sizeof(T)>::value) == 0;
}

/*
 * Calculate the number of bytes needed to allocate a single instance of type
 * |T| followed by |numExtra| contiguous instances of type |Extra|.  Return
 * false if the calculation overflowed.
 */
template <typename T, typename Extra>
[[nodiscard]] inline bool CalculateAllocSizeWithExtra(size_t numExtra,
                                                      size_t* bytesOut) {
  *bytesOut = sizeof(T) + numExtra * sizeof(Extra);
  return (numExtra & mozilla::tl::MulOverflowMask<sizeof(Extra)>::value) == 0 &&
         *bytesOut >= sizeof(T);
}

} /* namespace js */

template <class T>
static MOZ_ALWAYS_INLINE void js_delete(const T* p) {
  if (p) {
    p->~T();
    js_free(const_cast<T*>(p));
  }
}

template <class T>
static MOZ_ALWAYS_INLINE void js_delete_poison(const T* p) {
  if (p) {
    p->~T();
    memset(static_cast<void*>(const_cast<T*>(p)), 0x3B, sizeof(T));
    js_free(const_cast<T*>(p));
  }
}

template <class T>
static MOZ_ALWAYS_INLINE T* js_pod_arena_malloc(arena_id_t arena,
                                                size_t numElems) {
  size_t bytes;
  if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(numElems, &bytes))) {
    return nullptr;
  }
  return static_cast<T*>(js_arena_malloc(arena, bytes));
}

template <class T>
static MOZ_ALWAYS_INLINE T* js_pod_malloc(size_t numElems) {
  return js_pod_arena_malloc<T>(js::MallocArena, numElems);
}

template <class T>
static MOZ_ALWAYS_INLINE T* js_pod_arena_calloc(arena_id_t arena,
                                                size_t numElems) {
  size_t bytes;
  if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(numElems, &bytes))) {
    return nullptr;
  }
  return static_cast<T*>(js_arena_calloc(arena, bytes, 1));
}

template <class T>
static MOZ_ALWAYS_INLINE T* js_pod_calloc(size_t numElems) {
  return js_pod_arena_calloc<T>(js::MallocArena, numElems);
}

template <class T>
static MOZ_ALWAYS_INLINE T* js_pod_arena_realloc(arena_id_t arena, T* prior,
                                                 size_t oldSize,
                                                 size_t newSize) {
  MOZ_ASSERT(!(oldSize & mozilla::tl::MulOverflowMask<sizeof(T)>::value));
  size_t bytes;
  if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(newSize, &bytes))) {
    return nullptr;
  }
  return static_cast<T*>(js_arena_realloc(arena, prior, bytes));
}

template <class T>
static MOZ_ALWAYS_INLINE T* js_pod_realloc(T* prior, size_t oldSize,
                                           size_t newSize) {
  return js_pod_arena_realloc<T>(js::MallocArena, prior, oldSize, newSize);
}

namespace JS {

template <typename T>
struct DeletePolicy {
  constexpr DeletePolicy() = default;

  template <typename U>
  MOZ_IMPLICIT DeletePolicy(
      DeletePolicy<U> other,
      std::enable_if_t<std::is_convertible_v<U*, T*>, int> dummy = 0) {}

  void operator()(const T* ptr) { js_delete(const_cast<T*>(ptr)); }
};

struct FreePolicy {
  void operator()(const void* ptr) { js_free(const_cast<void*>(ptr)); }
};

using UniqueChars = mozilla::UniquePtr<char[], JS::FreePolicy>;
using UniqueTwoByteChars = mozilla::UniquePtr<char16_t[], JS::FreePolicy>;
using UniqueLatin1Chars = mozilla::UniquePtr<JS::Latin1Char[], JS::FreePolicy>;
using UniqueWideChars = mozilla::UniquePtr<wchar_t[], JS::FreePolicy>;

}  // namespace JS

/* sixgill annotation defines */
#ifndef HAVE_STATIC_ANNOTATIONS
#  define HAVE_STATIC_ANNOTATIONS
#  ifdef XGILL_PLUGIN
#    define STATIC_PRECONDITION(COND) __attribute__((precondition(#COND)))
#    define STATIC_PRECONDITION_ASSUME(COND) \
      __attribute__((precondition_assume(#COND)))
#    define STATIC_POSTCONDITION(COND) __attribute__((postcondition(#COND)))
#    define STATIC_POSTCONDITION_ASSUME(COND) \
      __attribute__((postcondition_assume(#COND)))
#    define STATIC_INVARIANT(COND) __attribute__((invariant(#COND)))
#    define STATIC_INVARIANT_ASSUME(COND) \
      __attribute__((invariant_assume(#COND)))
#    define STATIC_ASSUME(COND)                                          \
      JS_BEGIN_MACRO                                                     \
        __attribute__((assume_static(#COND), unused)) int STATIC_PASTE1( \
            assume_static_, __COUNTER__);                                \
      JS_END_MACRO
#  else                                       /* XGILL_PLUGIN */
#    define STATIC_PRECONDITION(COND)         /* nothing */
#    define STATIC_PRECONDITION_ASSUME(COND)  /* nothing */
#    define STATIC_POSTCONDITION(COND)        /* nothing */
#    define STATIC_POSTCONDITION_ASSUME(COND) /* nothing */
#    define STATIC_INVARIANT(COND)            /* nothing */
#    define STATIC_INVARIANT_ASSUME(COND)     /* nothing */
#    define STATIC_ASSUME(COND)    \
      JS_BEGIN_MACRO /* nothing */ \
      JS_END_MACRO
#  endif /* XGILL_PLUGIN */
#  define STATIC_SKIP_INFERENCE STATIC_INVARIANT(skip_inference())
#endif /* HAVE_STATIC_ANNOTATIONS */

#endif /* js_Utility_h */
