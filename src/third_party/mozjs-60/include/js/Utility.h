/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Utility_h
#define js_Utility_h

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/Compiler.h"
#include "mozilla/Move.h"
#include "mozilla/Scoped.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/WrappingOperations.h"

#include <stdlib.h>
#include <string.h>

#ifdef JS_OOM_DO_BACKTRACES
#include <execinfo.h>
#include <stdio.h>
#endif

#include "jstypes.h"

#include "mozmemory.h"

/* The public JS engine namespace. */
namespace JS {}

/* The mozilla-shared reusable template/utility namespace. */
namespace mozilla {}

/* The private JS engine namespace. */
namespace js {}

#define JS_STATIC_ASSERT(cond)           static_assert(cond, "JS_STATIC_ASSERT")
#define JS_STATIC_ASSERT_IF(cond, expr)  MOZ_STATIC_ASSERT_IF(cond, expr, "JS_STATIC_ASSERT_IF")

extern MOZ_NORETURN MOZ_COLD JS_PUBLIC_API(void)
JS_Assert(const char* s, const char* file, int ln);

/*
 * Custom allocator support for SpiderMonkey
 */
#if defined JS_USE_CUSTOM_ALLOCATOR
# include "jscustomallocator.h"
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
    THREAD_TYPE_NONE = 0,       // 0
    THREAD_TYPE_COOPERATING,    // 1
    THREAD_TYPE_WASM,           // 2
    THREAD_TYPE_ION,            // 3
    THREAD_TYPE_PARSE,          // 4
    THREAD_TYPE_COMPRESS,       // 5
    THREAD_TYPE_GCHELPER,       // 6
    THREAD_TYPE_GCPARALLEL,     // 7
    THREAD_TYPE_PROMISE_TASK,   // 8
    THREAD_TYPE_ION_FREE,       // 9
    THREAD_TYPE_WASM_TIER2,     // 10
    THREAD_TYPE_WORKER,         // 11
    THREAD_TYPE_MAX             // Used to check shell function arguments
};

namespace oom {

/*
 * Theads are tagged only in certain debug contexts.  Notably, to make testing
 * OOM in certain helper threads more effective, we allow restricting the OOM
 * testing to a certain helper thread type. This allows us to fail e.g. in
 * off-thread script parsing without causing an OOM in the active thread first.
 *
 * Getter/Setter functions to encapsulate mozilla::ThreadLocal, implementation
 * is in jsutil.cpp.
 */
# if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

// Define the range of threads tested by simulated OOM testing and the
// like. Testing worker threads is not supported.
const ThreadType FirstThreadTypeToTest = THREAD_TYPE_COOPERATING;
const ThreadType LastThreadTypeToTest = THREAD_TYPE_WASM_TIER2;

extern bool InitThreadType(void);
extern void SetThreadType(ThreadType);
extern JS_FRIEND_API(uint32_t) GetThreadType(void);

# else

inline bool InitThreadType(void) { return true; }
inline void SetThreadType(ThreadType t) {};
inline uint32_t GetThreadType(void) { return 0; }

# endif

} /* namespace oom */
} /* namespace js */

# if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

#ifdef JS_OOM_BREAKPOINT
#  if defined(_MSC_VER)
static MOZ_NEVER_INLINE void js_failedAllocBreakpoint() { __asm { }; }
#  else
static MOZ_NEVER_INLINE void js_failedAllocBreakpoint() { asm(""); }
#  endif
#define JS_OOM_CALL_BP_FUNC() js_failedAllocBreakpoint()
#else
#define JS_OOM_CALL_BP_FUNC() do {} while(0)
#endif

namespace js {
namespace oom {

/*
 * Out of memory testing support.  We provide various testing functions to
 * simulate OOM conditions and so we can test that they are handled correctly.
 */

extern JS_PUBLIC_DATA(uint32_t) targetThread;
extern JS_PUBLIC_DATA(uint64_t) maxAllocations;
extern JS_PUBLIC_DATA(uint64_t) counter;
extern JS_PUBLIC_DATA(bool) failAlways;

extern void
SimulateOOMAfter(uint64_t allocations, uint32_t thread, bool always);

extern void
ResetSimulatedOOM();

inline bool
IsThreadSimulatingOOM()
{
    return js::oom::targetThread && js::oom::targetThread == js::oom::GetThreadType();
}

inline bool
IsSimulatedOOMAllocation()
{
    return IsThreadSimulatingOOM() &&
           (counter == maxAllocations || (counter > maxAllocations && failAlways));
}

inline bool
ShouldFailWithOOM()
{
    if (!IsThreadSimulatingOOM())
        return false;

    counter++;
    if (IsSimulatedOOMAllocation()) {
        JS_OOM_CALL_BP_FUNC();
        return true;
    }
    return false;
}

inline bool
HadSimulatedOOM() {
    return counter >= maxAllocations;
}

/*
 * Out of stack space testing support, similar to OOM testing functions.
 */

extern JS_PUBLIC_DATA(uint32_t) stackTargetThread;
extern JS_PUBLIC_DATA(uint64_t) maxStackChecks;
extern JS_PUBLIC_DATA(uint64_t) stackCheckCounter;
extern JS_PUBLIC_DATA(bool) stackCheckFailAlways;

extern void
SimulateStackOOMAfter(uint64_t checks, uint32_t thread, bool always);

extern void
ResetSimulatedStackOOM();

inline bool
IsThreadSimulatingStackOOM()
{
    return js::oom::stackTargetThread && js::oom::stackTargetThread == js::oom::GetThreadType();
}

inline bool
IsSimulatedStackOOMCheck()
{
    return IsThreadSimulatingStackOOM() &&
           (stackCheckCounter == maxStackChecks || (stackCheckCounter > maxStackChecks && stackCheckFailAlways));
}

inline bool
ShouldFailWithStackOOM()
{
    if (!IsThreadSimulatingStackOOM())
        return false;

    stackCheckCounter++;
    if (IsSimulatedStackOOMCheck()) {
        JS_OOM_CALL_BP_FUNC();
        return true;
    }
    return false;
}

inline bool
HadSimulatedStackOOM()
{
    return stackCheckCounter >= maxStackChecks;
}

/*
 * Interrupt testing support, similar to OOM testing functions.
 */

extern JS_PUBLIC_DATA(uint32_t) interruptTargetThread;
extern JS_PUBLIC_DATA(uint64_t) maxInterruptChecks;
extern JS_PUBLIC_DATA(uint64_t) interruptCheckCounter;
extern JS_PUBLIC_DATA(bool) interruptCheckFailAlways;

extern void
SimulateInterruptAfter(uint64_t checks, uint32_t thread, bool always);

extern void
ResetSimulatedInterrupt();

inline bool
IsThreadSimulatingInterrupt()
{
    return js::oom::interruptTargetThread && js::oom::interruptTargetThread == js::oom::GetThreadType();
}

inline bool
IsSimulatedInterruptCheck()
{
    return IsThreadSimulatingInterrupt() &&
           (interruptCheckCounter == maxInterruptChecks || (interruptCheckCounter > maxInterruptChecks && interruptCheckFailAlways));
}

inline bool
ShouldFailWithInterrupt()
{
    if (!IsThreadSimulatingInterrupt())
        return false;

    interruptCheckCounter++;
    if (IsSimulatedInterruptCheck()) {
        JS_OOM_CALL_BP_FUNC();
        return true;
    }
    return false;
}

inline bool
HadSimulatedInterrupt()
{
    return interruptCheckCounter >= maxInterruptChecks;
}

} /* namespace oom */
} /* namespace js */

#  define JS_OOM_POSSIBLY_FAIL()                                              \
    do {                                                                      \
        if (js::oom::ShouldFailWithOOM())                                     \
            return nullptr;                                                   \
    } while (0)

#  define JS_OOM_POSSIBLY_FAIL_BOOL()                                         \
    do {                                                                      \
        if (js::oom::ShouldFailWithOOM())                                     \
            return false;                                                     \
    } while (0)

#  define JS_STACK_OOM_POSSIBLY_FAIL()                                        \
    do {                                                                      \
        if (js::oom::ShouldFailWithStackOOM())                                \
            return false;                                                     \
    } while (0)

#  define JS_STACK_OOM_POSSIBLY_FAIL_REPORT()                                 \
    do {                                                                      \
        if (js::oom::ShouldFailWithStackOOM()) {                              \
            ReportOverRecursed(cx);                                           \
            return false;                                                     \
        }                                                                     \
    } while (0)

#  define JS_INTERRUPT_POSSIBLY_FAIL()                                        \
    do {                                                                      \
        if (MOZ_UNLIKELY(js::oom::ShouldFailWithInterrupt())) {               \
            cx->interrupt_ = true;                                            \
            return cx->handleInterrupt();                                     \
        }                                                                     \
    } while (0)

# else

#  define JS_OOM_POSSIBLY_FAIL() do {} while(0)
#  define JS_OOM_POSSIBLY_FAIL_BOOL() do {} while(0)
#  define JS_STACK_OOM_POSSIBLY_FAIL() do {} while(0)
#  define JS_STACK_OOM_POSSIBLY_FAIL_REPORT() do {} while(0)
#  define JS_INTERRUPT_POSSIBLY_FAIL() do {} while(0)
namespace js {
namespace oom {
static inline bool IsSimulatedOOMAllocation() { return false; }
static inline bool ShouldFailWithOOM() { return false; }
} /* namespace oom */
} /* namespace js */

# endif /* DEBUG || JS_OOM_BREAKPOINT */

namespace js {

/* Disable OOM testing in sections which are not OOM safe. */
struct MOZ_RAII JS_PUBLIC_DATA(AutoEnterOOMUnsafeRegion)
{
    MOZ_NORETURN MOZ_COLD void crash(const char* reason);
    MOZ_NORETURN MOZ_COLD void crash(size_t size, const char* reason);

    using AnnotateOOMAllocationSizeCallback = void(*)(size_t);
    static AnnotateOOMAllocationSizeCallback annotateOOMSizeCallback;
    static void setAnnotateOOMAllocationSizeCallback(AnnotateOOMAllocationSizeCallback callback) {
        annotateOOMSizeCallback = callback;
    }

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    AutoEnterOOMUnsafeRegion()
      : oomEnabled_(oom::IsThreadSimulatingOOM() && oom::maxAllocations != UINT64_MAX),
        oomAfter_(0)
    {
        if (oomEnabled_) {
            MOZ_ALWAYS_TRUE(owner_.compareExchange(nullptr, this));
            oomAfter_ = int64_t(oom::maxAllocations) - int64_t(oom::counter);
            oom::maxAllocations = UINT64_MAX;
        }
    }

    ~AutoEnterOOMUnsafeRegion() {
        if (oomEnabled_) {
            MOZ_ASSERT(oom::maxAllocations == UINT64_MAX);
            int64_t maxAllocations = int64_t(oom::counter) + oomAfter_;
            MOZ_ASSERT(maxAllocations >= 0,
                       "alloc count + oom limit exceeds range, your oom limit is probably too large");
            oom::maxAllocations = uint64_t(maxAllocations);
            MOZ_ALWAYS_TRUE(owner_.compareExchange(this, nullptr));
        }
    }

  private:
    // Used to catch concurrent use from other threads.
    static mozilla::Atomic<AutoEnterOOMUnsafeRegion*> owner_;

    bool oomEnabled_;
    int64_t oomAfter_;
#endif
};

} /* namespace js */

// Malloc allocation.

namespace js {

extern JS_PUBLIC_DATA(arena_id_t) MallocArena;

extern void InitMallocAllocator();
extern void ShutDownMallocAllocator();

} /* namespace js */

static inline void* js_malloc(size_t bytes)
{
    JS_OOM_POSSIBLY_FAIL();
    return moz_arena_malloc(js::MallocArena, bytes);
}

static inline void* js_calloc(size_t bytes)
{
    JS_OOM_POSSIBLY_FAIL();
    return moz_arena_calloc(js::MallocArena, bytes, 1);
}

static inline void* js_calloc(size_t nmemb, size_t size)
{
    JS_OOM_POSSIBLY_FAIL();
    return moz_arena_calloc(js::MallocArena, nmemb, size);
}

static inline void* js_realloc(void* p, size_t bytes)
{
    // realloc() with zero size is not portable, as some implementations may
    // return nullptr on success and free |p| for this.  We assume nullptr
    // indicates failure and that |p| is still valid.
    MOZ_ASSERT(bytes != 0);

    JS_OOM_POSSIBLY_FAIL();
    return moz_arena_realloc(js::MallocArena, p, bytes);
}

static inline void js_free(void* p)
{
    // TODO: This should call |moz_arena_free(js::MallocArena, p)| but we
    // currently can't enforce that all memory freed here was allocated by
    // js_malloc().
    free(p);
}

JS_PUBLIC_API(char*) js_strdup(const char* s);
#endif/* JS_USE_CUSTOM_ALLOCATOR */

#include <new>

/*
 * Low-level memory management in SpiderMonkey:
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
 *     JSContext::{malloc_,realloc_,calloc_,new_}
 *     JSRuntime::{malloc_,realloc_,calloc_,new_}
 *
 *   These functions accumulate the number of bytes allocated which is used as
 *   part of the GC-triggering heuristic.
 *
 *   The difference between the JSContext and JSRuntime versions is that the
 *   cx version reports an out-of-memory error on OOM. (This follows from the
 *   general SpiderMonkey idiom that a JSContext-taking function reports its
 *   own errors.)
 *
 * - Otherwise, use js_malloc/js_realloc/js_calloc/js_new
 *
 * Deallocation:
 *
 * - Ordinarily, use js_free/js_delete.
 *
 * - For deallocations during GC finalization, use one of the following
 *   operations on the FreeOp provided to the finalizer:
 *
 *     FreeOp::{free_,delete_}
 *
 *   The advantage of these operations is that the memory is batched and freed
 *   on another thread.
 */

/*
 * Given a class which should provide a 'new' method, add
 * JS_DECLARE_NEW_METHODS (see js::MallocProvider for an example).
 *
 * Note: Do not add a ; at the end of a use of JS_DECLARE_NEW_METHODS,
 * or the build will break.
 */
#define JS_DECLARE_NEW_METHODS(NEWNAME, ALLOCATOR, QUALIFIERS) \
    template <class T, typename... Args> \
    QUALIFIERS T * \
    NEWNAME(Args&&... args) MOZ_HEAP_ALLOCATOR { \
        void* memory = ALLOCATOR(sizeof(T)); \
        return MOZ_LIKELY(memory) \
            ? new(memory) T(mozilla::Forward<Args>(args)...) \
            : nullptr; \
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
#define JS_DECLARE_MAKE_METHODS(MAKENAME, NEWNAME, QUALIFIERS)\
    template <class T, typename... Args> \
    QUALIFIERS mozilla::UniquePtr<T, JS::DeletePolicy<T>> \
    MAKENAME(Args&&... args) MOZ_HEAP_ALLOCATOR { \
        T* ptr = NEWNAME<T>(mozilla::Forward<Args>(args)...); \
        return mozilla::UniquePtr<T, JS::DeletePolicy<T>>(ptr); \
    }

JS_DECLARE_NEW_METHODS(js_new, js_malloc, static MOZ_ALWAYS_INLINE)

namespace js {

/*
 * Calculate the number of bytes needed to allocate |numElems| contiguous
 * instances of type |T|.  Return false if the calculation overflowed.
 */
template <typename T>
MOZ_MUST_USE inline bool
CalculateAllocSize(size_t numElems, size_t* bytesOut)
{
    *bytesOut = numElems * sizeof(T);
    return (numElems & mozilla::tl::MulOverflowMask<sizeof(T)>::value) == 0;
}

/*
 * Calculate the number of bytes needed to allocate a single instance of type
 * |T| followed by |numExtra| contiguous instances of type |Extra|.  Return
 * false if the calculation overflowed.
 */
template <typename T, typename Extra>
MOZ_MUST_USE inline bool
CalculateAllocSizeWithExtra(size_t numExtra, size_t* bytesOut)
{
    *bytesOut = sizeof(T) + numExtra * sizeof(Extra);
    return (numExtra & mozilla::tl::MulOverflowMask<sizeof(Extra)>::value) == 0 &&
           *bytesOut >= sizeof(T);
}

} /* namespace js */

template <class T>
static MOZ_ALWAYS_INLINE void
js_delete(const T* p)
{
    if (p) {
        p->~T();
        js_free(const_cast<T*>(p));
    }
}

template<class T>
static MOZ_ALWAYS_INLINE void
js_delete_poison(const T* p)
{
    if (p) {
        p->~T();
        memset(const_cast<T*>(p), 0x3B, sizeof(T));
        js_free(const_cast<T*>(p));
    }
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_malloc()
{
    return static_cast<T*>(js_malloc(sizeof(T)));
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_calloc()
{
    return static_cast<T*>(js_calloc(sizeof(T)));
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_malloc(size_t numElems)
{
    size_t bytes;
    if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(numElems, &bytes)))
        return nullptr;
    return static_cast<T*>(js_malloc(bytes));
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_calloc(size_t numElems)
{
    size_t bytes;
    if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(numElems, &bytes)))
        return nullptr;
    return static_cast<T*>(js_calloc(bytes));
}

template <class T>
static MOZ_ALWAYS_INLINE T*
js_pod_realloc(T* prior, size_t oldSize, size_t newSize)
{
    MOZ_ASSERT(!(oldSize & mozilla::tl::MulOverflowMask<sizeof(T)>::value));
    size_t bytes;
    if (MOZ_UNLIKELY(!js::CalculateAllocSize<T>(newSize, &bytes)))
        return nullptr;
    return static_cast<T*>(js_realloc(prior, bytes));
}

namespace js {

template<typename T>
struct ScopedFreePtrTraits
{
    typedef T* type;
    static T* empty() { return nullptr; }
    static void release(T* ptr) { js_free(ptr); }
};
SCOPED_TEMPLATE(ScopedJSFreePtr, ScopedFreePtrTraits)

template <typename T>
struct ScopedDeletePtrTraits : public ScopedFreePtrTraits<T>
{
    static void release(T* ptr) { js_delete(ptr); }
};
SCOPED_TEMPLATE(ScopedJSDeletePtr, ScopedDeletePtrTraits)

template <typename T>
struct ScopedReleasePtrTraits : public ScopedFreePtrTraits<T>
{
    static void release(T* ptr) { if (ptr) ptr->release(); }
};
SCOPED_TEMPLATE(ScopedReleasePtr, ScopedReleasePtrTraits)

} /* namespace js */

namespace JS {

template<typename T>
struct DeletePolicy
{
    constexpr DeletePolicy() {}

    template<typename U>
    MOZ_IMPLICIT DeletePolicy(DeletePolicy<U> other,
                              typename mozilla::EnableIf<mozilla::IsConvertible<U*, T*>::value,
                                                         int>::Type dummy = 0)
    {}

    void operator()(const T* ptr) {
        js_delete(const_cast<T*>(ptr));
    }
};

struct FreePolicy
{
    void operator()(const void* ptr) {
        js_free(const_cast<void*>(ptr));
    }
};

typedef mozilla::UniquePtr<char[], JS::FreePolicy> UniqueChars;
typedef mozilla::UniquePtr<char16_t[], JS::FreePolicy> UniqueTwoByteChars;

} // namespace JS

namespace js {

/* Integral types for all hash functions. */
typedef uint32_t HashNumber;
const unsigned HashNumberSizeBits = 32;

namespace detail {

/*
 * Given a raw hash code, h, return a number that can be used to select a hash
 * bucket.
 *
 * This function aims to produce as uniform an output distribution as possible,
 * especially in the most significant (leftmost) bits, even though the input
 * distribution may be highly nonrandom, given the constraints that this must
 * be deterministic and quick to compute.
 *
 * Since the leftmost bits of the result are best, the hash bucket index is
 * computed by doing ScrambleHashCode(h) / (2^32/N) or the equivalent
 * right-shift, not ScrambleHashCode(h) % N or the equivalent bit-mask.
 *
 * FIXME: OrderedHashTable uses a bit-mask; see bug 775896.
 */
inline HashNumber
ScrambleHashCode(HashNumber h)
{
    /*
     * Simply returning h would not cause any hash tables to produce wrong
     * answers. But it can produce pathologically bad performance: The caller
     * right-shifts the result, keeping only the highest bits. The high bits of
     * hash codes are very often completely entropy-free. (So are the lowest
     * bits.)
     *
     * So we use Fibonacci hashing, as described in Knuth, The Art of Computer
     * Programming, 6.4. This mixes all the bits of the input hash code h.
     *
     * The value of goldenRatio is taken from the hex
     * expansion of the golden ratio, which starts 1.9E3779B9....
     * This value is especially good if values with consecutive hash codes
     * are stored in a hash table; see Knuth for details.
     */
    static const HashNumber goldenRatio = 0x9E3779B9U;
    return mozilla::WrappingMultiply(h, goldenRatio);
}

} /* namespace detail */

} /* namespace js */

/* sixgill annotation defines */
#ifndef HAVE_STATIC_ANNOTATIONS
# define HAVE_STATIC_ANNOTATIONS
# ifdef XGILL_PLUGIN
#  define STATIC_PRECONDITION(COND)         __attribute__((precondition(#COND)))
#  define STATIC_PRECONDITION_ASSUME(COND)  __attribute__((precondition_assume(#COND)))
#  define STATIC_POSTCONDITION(COND)        __attribute__((postcondition(#COND)))
#  define STATIC_POSTCONDITION_ASSUME(COND) __attribute__((postcondition_assume(#COND)))
#  define STATIC_INVARIANT(COND)            __attribute__((invariant(#COND)))
#  define STATIC_INVARIANT_ASSUME(COND)     __attribute__((invariant_assume(#COND)))
#  define STATIC_ASSUME(COND)                        \
  JS_BEGIN_MACRO                                     \
    __attribute__((assume_static(#COND), unused))    \
    int STATIC_PASTE1(assume_static_, __COUNTER__);  \
  JS_END_MACRO
# else /* XGILL_PLUGIN */
#  define STATIC_PRECONDITION(COND)          /* nothing */
#  define STATIC_PRECONDITION_ASSUME(COND)   /* nothing */
#  define STATIC_POSTCONDITION(COND)         /* nothing */
#  define STATIC_POSTCONDITION_ASSUME(COND)  /* nothing */
#  define STATIC_INVARIANT(COND)             /* nothing */
#  define STATIC_INVARIANT_ASSUME(COND)      /* nothing */
#  define STATIC_ASSUME(COND)          JS_BEGIN_MACRO /* nothing */ JS_END_MACRO
# endif /* XGILL_PLUGIN */
# define STATIC_SKIP_INFERENCE STATIC_INVARIANT(skip_inference())
#endif /* HAVE_STATIC_ANNOTATIONS */

#endif /* js_Utility_h */
