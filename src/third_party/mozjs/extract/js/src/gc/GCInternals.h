/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definitions.
 */

#ifndef gc_GCInternals_h
#define gc_GCInternals_h

#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "gc/Cell.h"
#include "gc/GC.h"
#include "gc/GCContext.h"
#include "vm/GeckoProfiler.h"
#include "vm/HelperThreads.h"
#include "vm/JSContext.h"

namespace js {
namespace gc {

/*
 * There are a couple of classes here that serve mostly as "tokens" indicating
 * that a precondition holds. Some functions force the caller to possess such a
 * token because they require the precondition to hold, and it is better to make
 * the precondition explicit at the API entry point than to crash in an
 * assertion later on when it is relied upon.
 */

struct MOZ_RAII AutoAssertNoNurseryAlloc {
#ifdef DEBUG
  AutoAssertNoNurseryAlloc();
  ~AutoAssertNoNurseryAlloc();
#else
  AutoAssertNoNurseryAlloc() {}
#endif
};

/*
 * A class that serves as a token that the nursery in the current thread's zone
 * group is empty.
 */
class MOZ_RAII AutoAssertEmptyNursery {
 protected:
  JSContext* cx;

  mozilla::Maybe<AutoAssertNoNurseryAlloc> noAlloc;

  // Check that the nursery is empty.
  void checkCondition(JSContext* cx);

  // For subclasses that need to empty the nursery in their constructors.
  AutoAssertEmptyNursery() : cx(nullptr) {}

 public:
  explicit AutoAssertEmptyNursery(JSContext* cx) : cx(nullptr) {
    checkCondition(cx);
  }

  AutoAssertEmptyNursery(const AutoAssertEmptyNursery& other)
      : AutoAssertEmptyNursery(other.cx) {}
};

/*
 * Evict the nursery upon construction. Serves as a token indicating that the
 * nursery is empty. (See AutoAssertEmptyNursery, above.)
 */
class MOZ_RAII AutoEmptyNursery : public AutoAssertEmptyNursery {
 public:
  explicit AutoEmptyNursery(JSContext* cx);
};

// Abstract base class for exclusive heap access for tracing or GC.
class MOZ_RAII AutoHeapSession {
 public:
  ~AutoHeapSession();

 protected:
  AutoHeapSession(GCRuntime* gc, JS::HeapState state);

 private:
  AutoHeapSession(const AutoHeapSession&) = delete;
  void operator=(const AutoHeapSession&) = delete;

  GCRuntime* gc;
  JS::HeapState prevState;
  mozilla::Maybe<AutoGeckoProfilerEntry> profilingStackFrame;
};

class MOZ_RAII AutoGCSession : public AutoHeapSession {
 public:
  explicit AutoGCSession(GCRuntime* gc, JS::HeapState state)
      : AutoHeapSession(gc, state) {}
};

class MOZ_RAII AutoMajorGCProfilerEntry : public AutoGeckoProfilerEntry {
 public:
  explicit AutoMajorGCProfilerEntry(GCRuntime* gc);
};

class MOZ_RAII AutoTraceSession : public AutoHeapSession {
 public:
  explicit AutoTraceSession(JSRuntime* rt)
      : AutoHeapSession(&rt->gc, JS::HeapState::Tracing) {}
};

struct MOZ_RAII AutoFinishGC {
  explicit AutoFinishGC(JSContext* cx, JS::GCReason reason) {
    FinishGC(cx, reason);
  }
};

// This class should be used by any code that needs exclusive access to the heap
// in order to trace through it.
class MOZ_RAII AutoPrepareForTracing : private AutoFinishGC,
                                       public AutoTraceSession {
 public:
  explicit AutoPrepareForTracing(JSContext* cx)
      : AutoFinishGC(cx, JS::GCReason::PREPARE_FOR_TRACING),
        AutoTraceSession(cx->runtime()) {}
};

// This class should be used by any code that needs exclusive access to the heap
// in order to trace through it.
//
// This version also empties the nursery after finishing any ongoing GC.
class MOZ_RAII AutoEmptyNurseryAndPrepareForTracing : private AutoFinishGC,
                                                      public AutoEmptyNursery,
                                                      public AutoTraceSession {
 public:
  explicit AutoEmptyNurseryAndPrepareForTracing(JSContext* cx)
      : AutoFinishGC(cx, JS::GCReason::PREPARE_FOR_TRACING),
        AutoEmptyNursery(cx),
        AutoTraceSession(cx->runtime()) {}
};

/*
 * Temporarily disable incremental barriers.
 */
class AutoDisableBarriers {
 public:
  explicit AutoDisableBarriers(GCRuntime* gc);
  ~AutoDisableBarriers();

 private:
  GCRuntime* gc;
};

// Set compartments' maybeAlive flags if anything is marked while this class is
// live. This is used while marking roots.
class AutoUpdateLiveCompartments {
  GCRuntime* gc;

 public:
  explicit AutoUpdateLiveCompartments(GCRuntime* gc);
  ~AutoUpdateLiveCompartments();
};

class MOZ_RAII AutoRunParallelTask : public GCParallelTask {
  // This class takes a pointer to a member function of GCRuntime.
  using TaskFunc = JS_MEMBER_FN_PTR_TYPE(GCRuntime, void);

  TaskFunc func_;
  AutoLockHelperThreadState& lock_;

 public:
  AutoRunParallelTask(GCRuntime* gc, TaskFunc func, gcstats::PhaseKind phase,
                      GCUse use, AutoLockHelperThreadState& lock)
      : GCParallelTask(gc, phase, use), func_(func), lock_(lock) {
    gc->startTask(*this, lock_);
  }

  ~AutoRunParallelTask() { gc->joinTask(*this, lock_); }

  void run(AutoLockHelperThreadState& lock) override {
    AutoUnlockHelperThreadState unlock(lock);

    // The hazard analysis can't tell what the call to func_ will do but it's
    // not allowed to GC.
    JS::AutoSuppressGCAnalysis nogc;

    // Call pointer to member function on |gc|.
    JS_CALL_MEMBER_FN_PTR(gc, func_);
  }
};

GCAbortReason IsIncrementalGCUnsafe(JSRuntime* rt);

#ifdef JS_GC_ZEAL

class MOZ_RAII AutoStopVerifyingBarriers {
  GCRuntime* gc;
  bool restartPreVerifier;

 public:
  AutoStopVerifyingBarriers(JSRuntime* rt, bool isShutdown) : gc(&rt->gc) {
    if (gc->isVerifyPreBarriersEnabled()) {
      gc->endVerifyPreBarriers();
      restartPreVerifier = !isShutdown;
    } else {
      restartPreVerifier = false;
    }
  }

  ~AutoStopVerifyingBarriers() {
    // Nasty special case: verification runs a minor GC, which *may* nest
    // inside of an outer minor GC. This is not allowed by the
    // gc::Statistics phase tree. So we pause the "real" GC, if in fact one
    // is in progress.
    gcstats::PhaseKind outer = gc->stats().currentPhaseKind();
    if (outer != gcstats::PhaseKind::NONE) {
      gc->stats().endPhase(outer);
    }
    MOZ_ASSERT(gc->stats().currentPhaseKind() == gcstats::PhaseKind::NONE);

    if (restartPreVerifier) {
      gc->startVerifyPreBarriers();
    }

    if (outer != gcstats::PhaseKind::NONE) {
      gc->stats().beginPhase(outer);
    }
  }
};
#else
struct MOZ_RAII AutoStopVerifyingBarriers {
  AutoStopVerifyingBarriers(JSRuntime*, bool) {}
};
#endif /* JS_GC_ZEAL */

class MOZ_RAII AutoPoisonFreedJitCode {
  JS::GCContext* const gcx;

 public:
  explicit AutoPoisonFreedJitCode(JS::GCContext* gcx) : gcx(gcx) {}
  ~AutoPoisonFreedJitCode() { gcx->poisonJitCode(); }
};

// Set/restore the GCContext GC use flag for the current thread.

class MOZ_RAII AutoSetThreadGCUse {
 public:
  AutoSetThreadGCUse(JS::GCContext* gcx, GCUse use)
      : gcx(gcx), prevUse(gcx->gcUse_) {
    gcx->gcUse_ = use;
  }
  explicit AutoSetThreadGCUse(GCUse use)
      : AutoSetThreadGCUse(TlsGCContext.get(), use) {}

  ~AutoSetThreadGCUse() { gcx->gcUse_ = prevUse; }

 protected:
  JS::GCContext* gcx;
  GCUse prevUse;
};

template <GCUse Use>
class AutoSetThreadGCUseT : public AutoSetThreadGCUse {
 public:
  explicit AutoSetThreadGCUseT(JS::GCContext* gcx)
      : AutoSetThreadGCUse(gcx, Use) {}
  AutoSetThreadGCUseT() : AutoSetThreadGCUseT(TlsGCContext.get()) {}
};

using AutoSetThreadIsPerformingGC = AutoSetThreadGCUseT<GCUse::Unspecified>;
using AutoSetThreadIsMarking = AutoSetThreadGCUseT<GCUse::Marking>;
using AutoSetThreadIsFinalizing = AutoSetThreadGCUseT<GCUse::Finalizing>;

class AutoSetThreadIsSweeping : public AutoSetThreadGCUseT<GCUse::Sweeping> {
 public:
  explicit AutoSetThreadIsSweeping(JS::GCContext* gcx,
                                   JS::Zone* sweepZone = nullptr)
      : AutoSetThreadGCUseT(gcx) {
#ifdef DEBUG
    prevZone = gcx->gcSweepZone_;
    gcx->gcSweepZone_ = sweepZone;
#endif
  }
  explicit AutoSetThreadIsSweeping(JS::Zone* sweepZone = nullptr)
      : AutoSetThreadIsSweeping(TlsGCContext.get(), sweepZone) {}

  ~AutoSetThreadIsSweeping() {
#ifdef DEBUG
    MOZ_ASSERT_IF(prevUse == GCUse::None, !prevZone);
    gcx->gcSweepZone_ = prevZone;
#endif
  }

 private:
#ifdef DEBUG
  JS::Zone* prevZone;
#endif
};

#ifdef JSGC_HASH_TABLE_CHECKS
void CheckHashTablesAfterMovingGC(JSRuntime* rt);
void CheckHeapAfterGC(JSRuntime* rt);
#endif

struct MovingTracer final : public GenericTracerImpl<MovingTracer> {
  explicit MovingTracer(JSRuntime* rt);

 private:
  template <typename T>
  void onEdge(T** thingp, const char* name);
  friend class GenericTracerImpl<MovingTracer>;
};

struct MinorSweepingTracer final
    : public GenericTracerImpl<MinorSweepingTracer> {
  explicit MinorSweepingTracer(JSRuntime* rt);

 private:
  template <typename T>
  void onEdge(T** thingp, const char* name);
  friend class GenericTracerImpl<MinorSweepingTracer>;
};

extern void DelayCrossCompartmentGrayMarking(GCMarker* maybeMarker,
                                             JSObject* src);

inline bool IsOOMReason(JS::GCReason reason) {
  return reason == JS::GCReason::LAST_DITCH ||
         reason == JS::GCReason::MEM_PRESSURE;
}

void* AllocateCellInGC(JS::Zone* zone, AllocKind thingKind);

void ReadProfileEnv(const char* envName, const char* helpText, bool* enableOut,
                    bool* workersOut, mozilla::TimeDuration* thresholdOut);

bool ShouldPrintProfile(JSRuntime* runtime, bool enable, bool workers,
                        mozilla::TimeDuration threshold,
                        mozilla::TimeDuration duration);

} /* namespace gc */
} /* namespace js */

#endif /* gc_GCInternals_h */
