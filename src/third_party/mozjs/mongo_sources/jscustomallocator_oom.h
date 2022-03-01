/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Defines, Enums, Functions, and Types extract from include/js/Utility.h
 * to support MongoDB's jscustomallocator.h
 */

#pragma once

#include <cstdlib>
#include <mozilla/Atomics.h>

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
    THREAD_TYPE_PARSE,                 // 5
    THREAD_TYPE_COMPRESS,              // 6
    THREAD_TYPE_GCPARALLEL,            // 7
    THREAD_TYPE_PROMISE_TASK,          // 8
    THREAD_TYPE_ION_FREE,              // 9
    THREAD_TYPE_WASM_GENERATOR_TIER2,  // 10
    THREAD_TYPE_WORKER,                // 11
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
# if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

// Define the range of threads tested by simulated OOM testing and the
// like. Testing worker threads is not supported.
const ThreadType FirstThreadTypeToTest = THREAD_TYPE_MAIN;
const ThreadType LastThreadTypeToTest = THREAD_TYPE_WASM_GENERATOR_TIER2;

extern bool InitThreadType(void);
extern void SetThreadType(ThreadType);
extern JS_PUBLIC_API uint32_t GetThreadType(void);

# else

inline bool InitThreadType(void) { return true; }
inline void SetThreadType(ThreadType t) {};
inline uint32_t GetThreadType(void) { return 0; }
inline uint32_t GetAllocationThreadType(void) { return 0; }
inline uint32_t GetStackCheckThreadType(void) { return 0; }
inline uint32_t GetInterruptCheckThreadType(void) { return 0; }

# endif

} /* namespace oom */
} /* namespace js */

# if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

#    ifdef JS_OOM_BREAKPOINT
#      if defined(_MSC_VER)
static MOZ_NEVER_INLINE void js_failedAllocBreakpoint() { __asm {}; }
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
