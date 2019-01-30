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

#include "jit/CompileInfo.h"
#include "jit/JitAllocPolicy.h"
#include "jit/JitCompartment.h"
#include "jit/MIR.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/RegisterSets.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"

namespace js {
namespace jit {

class MIRGraph;
class OptimizationInfo;

class MIRGenerator
{
  public:
    MIRGenerator(CompileCompartment* compartment, const JitCompileOptions& options,
                 TempAllocator* alloc, MIRGraph* graph,
                 const CompileInfo* info, const OptimizationInfo* optimizationInfo);

    void initMinWasmHeapLength(uint32_t init) {
        minWasmHeapLength_ = init;
    }

    TempAllocator& alloc() {
        return *alloc_;
    }
    MIRGraph& graph() {
        return *graph_;
    }
    MOZ_MUST_USE bool ensureBallast() {
        return alloc().ensureBallast();
    }
    const JitRuntime* jitRuntime() const {
        return runtime->jitRuntime();
    }
    const CompileInfo& info() const {
        return *info_;
    }
    const OptimizationInfo& optimizationInfo() const {
        return *optimizationInfo_;
    }
    bool hasProfilingScripts() const {
        return runtime && runtime->profilingScripts();
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
    mozilla::GenericErrorResult<AbortReason> abort(AbortReason r);
    mozilla::GenericErrorResult<AbortReason>
    abort(AbortReason r, const char* message, ...) MOZ_FORMAT_PRINTF(3, 4);

    mozilla::GenericErrorResult<AbortReason>
    abortFmt(AbortReason r, const char* message, va_list ap) MOZ_FORMAT_PRINTF(3, 0);

    // Collect the evaluation result of phases after IonBuilder, such that
    // off-thread compilation can report what error got encountered.
    void setOffThreadStatus(AbortReasonOr<Ok> result) {
        MOZ_ASSERT(offThreadStatus_.isOk());
        offThreadStatus_ = result;
    }
    AbortReasonOr<Ok> getOffThreadStatus() const {
        return offThreadStatus_;
    }

    MOZ_MUST_USE bool instrumentedProfiling() {
        if (!instrumentedProfilingIsCached_) {
            instrumentedProfiling_ = runtime->geckoProfiler().enabled();
            instrumentedProfilingIsCached_ = true;
        }
        return instrumentedProfiling_;
    }

    bool isProfilerInstrumentationEnabled() {
        return !compilingWasm() && instrumentedProfiling();
    }

    bool isOptimizationTrackingEnabled() {
        return isProfilerInstrumentationEnabled() && !info().isAnalysis() &&
               !JitOptions.disableOptimizationTracking;
    }

    bool stringsCanBeInNursery() const {
        return stringsCanBeInNursery_;
    }

    bool safeForMinorGC() const {
        return safeForMinorGC_;
    }
    void setNotSafeForMinorGC() {
        safeForMinorGC_ = false;
    }

    // Whether the active thread is trying to cancel this build.
    bool shouldCancel(const char* why) {
        return cancelBuild_;
    }
    void cancel() {
        cancelBuild_ = true;
    }

    bool compilingWasm() const {
        return info_->compilingWasm();
    }

    uint32_t wasmMaxStackArgBytes() const {
        MOZ_ASSERT(compilingWasm());
        return wasmMaxStackArgBytes_;
    }
    void initWasmMaxStackArgBytes(uint32_t n) {
        MOZ_ASSERT(compilingWasm());
        MOZ_ASSERT(wasmMaxStackArgBytes_ == 0);
        wasmMaxStackArgBytes_ = n;
    }
    uint32_t minWasmHeapLength() const {
        return minWasmHeapLength_;
    }

    void setNeedsOverrecursedCheck() {
        needsOverrecursedCheck_ = true;
    }
    bool needsOverrecursedCheck() const {
        return needsOverrecursedCheck_;
    }

    void setNeedsStaticStackAlignment() {
        needsStaticStackAlignment_ = true;
    }
    bool needsStaticStackAlignment() const {
        return needsOverrecursedCheck_;
    }

    // Traverses the graph to find if there's any SIMD instruction. Costful but
    // the value is cached, so don't worry about calling it several times.
    bool usesSimd();

    bool modifiesFrameArguments() const {
        return modifiesFrameArguments_;
    }

    typedef Vector<ObjectGroup*, 0, JitAllocPolicy> ObjectGroupVector;

    // When aborting with AbortReason::PreliminaryObjects, all groups with
    // preliminary objects which haven't been analyzed yet.
    const ObjectGroupVector& abortedPreliminaryGroups() const {
        return abortedPreliminaryGroups_;
    }

  public:
    CompileCompartment* compartment;
    CompileRuntime* runtime;

  protected:
    const CompileInfo* info_;
    const OptimizationInfo* optimizationInfo_;
    TempAllocator* alloc_;
    MIRGraph* graph_;
    AbortReasonOr<Ok> offThreadStatus_;
    ObjectGroupVector abortedPreliminaryGroups_;
    mozilla::Atomic<bool, mozilla::Relaxed> cancelBuild_;

    uint32_t wasmMaxStackArgBytes_;
    bool needsOverrecursedCheck_;
    bool needsStaticStackAlignment_;
    bool usesSimd_;
    bool cachedUsesSimd_;

    // Keep track of whether frame arguments are modified during execution.
    // RegAlloc needs to know this as spilling values back to their register
    // slots is not compatible with that.
    bool modifiesFrameArguments_;

    bool instrumentedProfiling_;
    bool instrumentedProfilingIsCached_;
    bool safeForMinorGC_;
    bool stringsCanBeInNursery_;

    void addAbortedPreliminaryGroup(ObjectGroup* group);

    uint32_t minWasmHeapLength_;

#if defined(JS_ION_PERF)
    WasmPerfSpewer wasmPerfSpewer_;

  public:
    WasmPerfSpewer& perfSpewer() { return wasmPerfSpewer_; }
#endif

  public:
    const JitCompileOptions options;

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
