/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitOptions_h
#define jit_JitOptions_h

#include "mozilla/Maybe.h"

#include "jit/IonTypes.h"
#include "js/TypeDecls.h"

namespace js {
namespace jit {

// Longer scripts can only be compiled off thread, as these compilations
// can be expensive and stall the active thread for too long.
static const uint32_t MAX_ACTIVE_THREAD_SCRIPT_SIZE = 2 * 1000;
static const uint32_t MAX_ACTIVE_THREAD_LOCALS_AND_ARGS = 256;

// Possible register allocators which may be used.
enum IonRegisterAllocator {
    RegisterAllocator_Backtracking,
    RegisterAllocator_Testbed,
    RegisterAllocator_Stupid
};

static inline mozilla::Maybe<IonRegisterAllocator>
LookupRegisterAllocator(const char* name)
{
    if (!strcmp(name, "backtracking"))
        return mozilla::Some(RegisterAllocator_Backtracking);
    if (!strcmp(name, "testbed"))
        return mozilla::Some(RegisterAllocator_Testbed);
    if (!strcmp(name, "stupid"))
        return mozilla::Some(RegisterAllocator_Stupid);
    return mozilla::Nothing();
}

struct DefaultJitOptions
{
    bool checkGraphConsistency;
#ifdef CHECK_OSIPOINT_REGISTERS
    bool checkOsiPointRegisters;
#endif
    bool checkRangeAnalysis;
    bool runExtraChecks;
    bool disableInlineBacktracking;
    bool disableAma;
    bool disableEaa;
    bool disableEagerSimdUnbox;
    bool disableEdgeCaseAnalysis;
    bool disableFlowAA;
    bool disableGvn;
    bool disableInlining;
    bool disableLicm;
    bool disableLoopUnrolling;
    bool disableOptimizationTracking;
    bool disablePgo;
    bool disableInstructionReordering;
    bool disableRangeAnalysis;
    bool disableRecoverIns;
    bool disableScalarReplacement;
    bool disableCacheIR;
    bool disableSharedStubs;
    bool disableSincos;
    bool disableSink;
    bool eagerCompilation;
    bool forceInlineCaches;
    bool fullDebugChecks;
    bool limitScriptSize;
    bool osr;
    bool asmJSAtomicsEnable;
    bool wasmFoldOffsets;
    bool wasmDelayTier2;
    bool ionInterruptWithoutSignals;
    bool simulatorAlwaysInterrupt;
    uint32_t baselineWarmUpThreshold;
    uint32_t exceptionBailoutThreshold;
    uint32_t frequentBailoutThreshold;
    uint32_t maxStackArgs;
    uint32_t osrPcMismatchesBeforeRecompile;
    uint32_t smallFunctionMaxBytecodeLength_;
    uint32_t jumpThreshold;
    uint32_t branchPruningHitCountFactor;
    uint32_t branchPruningInstFactor;
    uint32_t branchPruningBlockSpanFactor;
    uint32_t branchPruningEffectfulInstFactor;
    uint32_t branchPruningThreshold;
    uint32_t wasmBatchIonThreshold;
    uint32_t wasmBatchBaselineThreshold;
    mozilla::Maybe<uint32_t> forcedDefaultIonWarmUpThreshold;
    mozilla::Maybe<uint32_t> forcedDefaultIonSmallFunctionWarmUpThreshold;
    mozilla::Maybe<IonRegisterAllocator> forcedRegisterAllocator;

    // Spectre mitigation flags. Each mitigation has its own flag in order to
    // measure the effectiveness of each mitigation with various proof of
    // concept.
    bool spectreIndexMasking;
    bool spectreObjectMitigationsBarriers;
    bool spectreObjectMitigationsMisc;
    bool spectreStringMitigations;
    bool spectreValueMasking;
    bool spectreJitToCxxCalls;

    // The options below affect the rest of the VM, and not just the JIT.
    bool disableUnboxedObjects;

    DefaultJitOptions();
    bool isSmallFunction(JSScript* script) const;
    void setEagerCompilation();
    void setCompilerWarmUpThreshold(uint32_t warmUpThreshold);
    void resetCompilerWarmUpThreshold();
    void enableGvn(bool val);
};

extern DefaultJitOptions JitOptions;

} // namespace jit
} // namespace js

#endif /* jit_JitOptions_h */
