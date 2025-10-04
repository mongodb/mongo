/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

// Possible register allocators which may be used.
enum IonRegisterAllocator {
  RegisterAllocator_Backtracking,
  RegisterAllocator_Testbed,
};

// Which register to use as base register to access stack slots: frame pointer,
// stack pointer, or whichever is the default for this platform. See comment
// for baseRegForLocals in JitOptions.cpp for more information.
enum class BaseRegForAddress { Default, FP, SP };

enum class UseMonomorphicInlining : uint8_t {
  Default,
  Always,
  Never,
};

static inline mozilla::Maybe<IonRegisterAllocator> LookupRegisterAllocator(
    const char* name) {
  if (!strcmp(name, "backtracking")) {
    return mozilla::Some(RegisterAllocator_Backtracking);
  }
  if (!strcmp(name, "testbed")) {
    return mozilla::Some(RegisterAllocator_Testbed);
  }
  return mozilla::Nothing();
}

struct DefaultJitOptions {
  bool checkGraphConsistency;
#ifdef CHECK_OSIPOINT_REGISTERS
  bool checkOsiPointRegisters;
#endif
  bool checkRangeAnalysis;
  bool runExtraChecks;
  bool disableJitBackend;
  bool disableJitHints;
  bool disableAma;
  bool disableEaa;
  bool disableEdgeCaseAnalysis;
  bool disableGvn;
  bool disableInlining;
  bool disableLicm;
  bool disablePruning;
  bool disableInstructionReordering;
  bool disableIteratorIndices;
  bool disableMarkLoadsUsedAsPropertyKeys;
  bool disableRangeAnalysis;
  bool disableRecoverIns;
  bool disableScalarReplacement;
  bool disableCacheIR;
  bool disableSink;
  bool disableRedundantShapeGuards;
  bool disableRedundantGCBarriers;
  bool disableBailoutLoopCheck;
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  bool portableBaselineInterpreter;
#endif
  bool baselineInterpreter;
  bool baselineJit;
  bool ion;
  bool jitForTrustedPrincipals;
  bool nativeRegExp;
  bool forceInlineCaches;
  bool forceMegamorphicICs;
  bool fullDebugChecks;
  bool limitScriptSize;
  bool osr;
  bool wasmFoldOffsets;
  bool wasmDelayTier2;
  bool lessDebugCode;
  bool onlyInlineSelfHosted;
  bool enableICFramePointers;
  bool enableWasmJitExit;
  bool enableWasmJitEntry;
  bool enableWasmIonFastCalls;
#ifdef WASM_CODEGEN_DEBUG
  bool enableWasmImportCallSpew;
  bool enableWasmFuncCallSpew;
#endif
  bool emitInterpreterEntryTrampoline;
  uint32_t baselineInterpreterWarmUpThreshold;
  uint32_t baselineJitWarmUpThreshold;
  uint32_t trialInliningWarmUpThreshold;
  uint32_t trialInliningInitialWarmUpCount;
  UseMonomorphicInlining monomorphicInlining = UseMonomorphicInlining::Default;
  uint32_t normalIonWarmUpThreshold;
  uint32_t regexpWarmUpThreshold;
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  uint32_t portableBaselineInterpreterWarmUpThreshold;
#endif
  uint32_t exceptionBailoutThreshold;
  uint32_t frequentBailoutThreshold;
  uint32_t maxStackArgs;
  uint32_t osrPcMismatchesBeforeRecompile;
  uint32_t smallFunctionMaxBytecodeLength;
  uint32_t inliningEntryThreshold;
  uint32_t jumpThreshold;
  uint32_t branchPruningHitCountFactor;
  uint32_t branchPruningInstFactor;
  uint32_t branchPruningBlockSpanFactor;
  uint32_t branchPruningEffectfulInstFactor;
  uint32_t branchPruningThreshold;
  uint32_t ionMaxScriptSize;
  uint32_t ionMaxScriptSizeMainThread;
  uint32_t ionMaxLocalsAndArgs;
  uint32_t ionMaxLocalsAndArgsMainThread;
  uint32_t wasmBatchBaselineThreshold;
  uint32_t wasmBatchIonThreshold;
  mozilla::Maybe<IonRegisterAllocator> forcedRegisterAllocator;

  // Spectre mitigation flags. Each mitigation has its own flag in order to
  // measure the effectiveness of each mitigation with various proof of
  // concept.
  bool spectreIndexMasking;
  bool spectreObjectMitigations;
  bool spectreStringMitigations;
  bool spectreValueMasking;
  bool spectreJitToCxxCalls;

  bool writeProtectCode;

  bool supportsUnalignedAccesses;
  BaseRegForAddress baseRegForLocals;

  // Irregexp shim flags
  bool correctness_fuzzer_suppressions;
  bool enable_regexp_unaligned_accesses;
  bool js_regexp_modifiers;
  bool js_regexp_duplicate_named_groups;
  bool regexp_possessive_quantifier;
  bool regexp_optimization;
  bool regexp_peephole_optimization;
  bool regexp_tier_up;
  bool trace_regexp_assembler;
  bool trace_regexp_bytecodes;
  bool trace_regexp_parser;
  bool trace_regexp_peephole_optimization;

  DefaultJitOptions();
  bool isSmallFunction(JSScript* script) const;
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  void setEagerPortableBaselineInterpreter();
#endif
  void setEagerBaselineCompilation();
  void setEagerIonCompilation();
  void setNormalIonWarmUpThreshold(uint32_t warmUpThreshold);
  void resetNormalIonWarmUpThreshold();
  void enableGvn(bool val);
  void setFastWarmUp();

  void maybeSetWriteProtectCode(bool val);

  bool eagerIonCompilation() const { return normalIonWarmUpThreshold == 0; }
};

extern DefaultJitOptions JitOptions;

inline bool HasJitBackend() {
#if defined(JS_CODEGEN_NONE)
  return false;
#else
  return !JitOptions.disableJitBackend;
#endif
}

inline bool IsBaselineInterpreterEnabled() {
  return HasJitBackend() && JitOptions.baselineInterpreter;
}

#ifdef ENABLE_PORTABLE_BASELINE_INTERP
inline bool IsPortableBaselineInterpreterEnabled() {
  return JitOptions.portableBaselineInterpreter;
}
#else
inline bool IsPortableBaselineInterpreterEnabled() { return false; }
#endif

inline bool TooManyActualArguments(size_t nargs) {
  return nargs > JitOptions.maxStackArgs;
}

}  // namespace jit

extern mozilla::Atomic<bool> fuzzingSafe;

static inline bool IsFuzzing() {
#ifdef FUZZING
  return true;
#else
  return fuzzingSafe;
#endif
}

}  // namespace js

#endif /* jit_JitOptions_h */
