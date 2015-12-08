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
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

class MBasicBlock;
class MIRGraph;
class MStart;
class OptimizationInfo;

class MIRGenerator
{
  public:
    MIRGenerator(CompileCompartment* compartment, const JitCompileOptions& options,
                 TempAllocator* alloc, MIRGraph* graph,
                 CompileInfo* info, const OptimizationInfo* optimizationInfo);

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
    CompileInfo& info() {
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
    void initMinAsmJSHeapLength(uint32_t len) {
        MOZ_ASSERT(minAsmJSHeapLength_ == 0);
        minAsmJSHeapLength_ = len;
    }
    uint32_t minAsmJSHeapLength() const {
        return minAsmJSHeapLength_;
    }

    bool modifiesFrameArguments() const {
        return modifiesFrameArguments_;
    }

    typedef Vector<ObjectGroup*, 0, JitAllocPolicy> ObjectGroupVector;

    // When abortReason() == AbortReason_NewScriptProperties, all types which
    // the new script properties analysis hasn't been performed on yet.
    const ObjectGroupVector& abortedNewScriptPropertiesGroups() const {
        return abortedNewScriptPropertiesGroups_;
    }

  public:
    CompileCompartment* compartment;

  protected:
    CompileInfo* info_;
    const OptimizationInfo* optimizationInfo_;
    TempAllocator* alloc_;
    JSFunction* fun_;
    uint32_t nslots_;
    MIRGraph* graph_;
    AbortReason abortReason_;
    bool shouldForceAbort_; // Force AbortReason_Disable
    ObjectGroupVector abortedNewScriptPropertiesGroups_;
    bool error_;
    mozilla::Atomic<bool, mozilla::Relaxed>* pauseBuild_;
    mozilla::Atomic<bool, mozilla::Relaxed> cancelBuild_;

    uint32_t maxAsmJSStackArgBytes_;
    bool performsCall_;
    bool usesSimd_;
    bool usesSimdCached_;
    uint32_t minAsmJSHeapLength_;

    // Keep track of whether frame arguments are modified during execution.
    // RegAlloc needs to know this as spilling values back to their register
    // slots is not compatible with that.
    bool modifiesFrameArguments_;

    bool instrumentedProfiling_;
    bool instrumentedProfilingIsCached_;

    // List of nursery objects used by this compilation. Can be traced by a
    // minor GC while compilation happens off-thread. This Vector should only
    // be accessed on the main thread (IonBuilder, nursery GC or
    // CodeGenerator::link).
    ObjectVector nurseryObjects_;

    void addAbortedNewScriptPropertiesGroup(ObjectGroup* type);
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

    void traceNurseryObjects(JSTracer* trc);

    const ObjectVector& nurseryObjects() const {
        return nurseryObjects_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_MIRGenerator_h */
