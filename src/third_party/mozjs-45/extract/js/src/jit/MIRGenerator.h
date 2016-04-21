/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MIRGenerator_h
#define jit_MIRGenerator_h

// This file declares the data structures used to build a control-flow graph
// containing MIR.

#include "mozilla/Atomics.h"

#include <stdarg.h>

#include "jscntxt.h"
#include "jscompartment.h"

#include "jit/CompileInfo.h"
#include "jit/JitAllocPolicy.h"
#include "jit/JitCompartment.h"
#include "jit/MIR.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

class MIRGraph;
class OptimizationInfo;

class MIRGenerator
{
  public:
    MIRGenerator(CompileCompartment* compartment, const JitCompileOptions& options,
                 TempAllocator* alloc, MIRGraph* graph,
                 const CompileInfo* info, const OptimizationInfo* optimizationInfo,
                 bool usesSignalHandlersForAsmJSOOB = false);

    TempAllocator& alloc() {
        return *alloc_;
    }
    MIRGraph& graph() {
        return *graph_;
    }
    bool ensureBallast() {
        return alloc().ensureBallast();
    }
    const JitRuntime* jitRuntime() const {
        return GetJitContext()->runtime->jitRuntime();
    }
    const CompileInfo& info() const {
        return *info_;
    }
    const OptimizationInfo& optimizationInfo() const {
        return *optimizationInfo_;
    }

    template <typename T>
    T* allocate(size_t count = 1) {
        size_t bytes;
        if (MOZ_UNLIKELY(!CalculateAllocSize<T>(count, &bytes)))
            return nullptr;
        return static_cast<T*>(alloc().allocate(bytes));
    }

    // Set an error state and prints a message. Returns false so errors can be
    // propagated up.
    bool abort(const char* message, ...);
    bool abortFmt(const char* message, va_list ap);

    bool errored() const {
        return error_;
    }

    bool instrumentedProfiling() {
        if (!instrumentedProfilingIsCached_) {
            instrumentedProfiling_ = GetJitContext()->runtime->spsProfiler().enabled();
            instrumentedProfilingIsCached_ = true;
        }
        return instrumentedProfiling_;
    }

    bool isProfilerInstrumentationEnabled() {
        return !compilingAsmJS() && instrumentedProfiling();
    }

    bool isOptimizationTrackingEnabled() {
        return isProfilerInstrumentationEnabled() && !info().isAnalysis();
    }

    bool safeForMinorGC() const {
        return safeForMinorGC_;
    }
    void setNotSafeForMinorGC() {
        safeForMinorGC_ = false;
    }

    // Whether the main thread is trying to cancel this build.
    bool shouldCancel(const char* why) {
        maybePause();
        return cancelBuild_;
    }
    void cancel() {
        cancelBuild_ = true;
    }

    void maybePause() {
        if (pauseBuild_ && *pauseBuild_)
            PauseCurrentHelperThread();
    }
    void setPauseFlag(mozilla::Atomic<bool, mozilla::Relaxed>* pauseBuild) {
        pauseBuild_ = pauseBuild;
    }

    void disable() {
        abortReason_ = AbortReason_Disable;
    }
    AbortReason abortReason() {
        return abortReason_;
    }

    bool compilingAsmJS() const {
        return info_->compilingAsmJS();
    }

    uint32_t maxAsmJSStackArgBytes() const {
        MOZ_ASSERT(compilingAsmJS());
        return maxAsmJSStackArgBytes_;
    }
    uint32_t resetAsmJSMaxStackArgBytes() {
        MOZ_ASSERT(compilingAsmJS());
        uint32_t old = maxAsmJSStackArgBytes_;
        maxAsmJSStackArgBytes_ = 0;
        return old;
    }
    void setAsmJSMaxStackArgBytes(uint32_t n) {
        MOZ_ASSERT(compilingAsmJS());
        maxAsmJSStackArgBytes_ = n;
    }
    void setPerformsCall() {
        performsCall_ = true;
    }
    bool performsCall() const {
        return performsCall_;
    }
    // Traverses the graph to find if there's any SIMD instruction. Costful but
    // the value is cached, so don't worry about calling it several times.
    bool usesSimd();

    bool modifiesFrameArguments() const {
        return modifiesFrameArguments_;
    }

    typedef Vector<ObjectGroup*, 0, JitAllocPolicy> ObjectGroupVector;

    // When abortReason() == AbortReason_PreliminaryObjects, all groups with
    // preliminary objects which haven't been analyzed yet.
    const ObjectGroupVector& abortedPreliminaryGroups() const {
        return abortedPreliminaryGroups_;
    }

  public:
    CompileCompartment* compartment;

  protected:
    const CompileInfo* info_;
    const OptimizationInfo* optimizationInfo_;
    TempAllocator* alloc_;
    JSFunction* fun_;
    uint32_t nslots_;
    MIRGraph* graph_;
    AbortReason abortReason_;
    bool shouldForceAbort_; // Force AbortReason_Disable
    ObjectGroupVector abortedPreliminaryGroups_;
    bool error_;
    mozilla::Atomic<bool, mozilla::Relaxed>* pauseBuild_;
    mozilla::Atomic<bool, mozilla::Relaxed> cancelBuild_;

    uint32_t maxAsmJSStackArgBytes_;
    bool performsCall_;
    bool usesSimd_;
    bool usesSimdCached_;

    // Keep track of whether frame arguments are modified during execution.
    // RegAlloc needs to know this as spilling values back to their register
    // slots is not compatible with that.
    bool modifiesFrameArguments_;

    bool instrumentedProfiling_;
    bool instrumentedProfilingIsCached_;
    bool safeForMinorGC_;

    void addAbortedPreliminaryGroup(ObjectGroup* group);

#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
    bool usesSignalHandlersForAsmJSOOB_;
#endif

    void setForceAbort() {
        shouldForceAbort_ = true;
    }
    bool shouldForceAbort() {
        return shouldForceAbort_;
    }

#if defined(JS_ION_PERF)
    AsmJSPerfSpewer asmJSPerfSpewer_;

  public:
    AsmJSPerfSpewer& perfSpewer() { return asmJSPerfSpewer_; }
#endif

  public:
    const JitCompileOptions options;

    bool needsAsmJSBoundsCheckBranch(const MAsmJSHeapAccess* access) const;
    size_t foldableOffsetRange(const MAsmJSHeapAccess* access) const;

  private:
    GraphSpewer gs_;

  public:
    GraphSpewer& graphSpewer() {
        return gs_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_MIRGenerator_h */
