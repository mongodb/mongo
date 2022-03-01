/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitOptions.h"

#include <cstdlib>
#include <type_traits>

#include "vm/JSFunction.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

namespace js {
namespace jit {

DefaultJitOptions JitOptions;

static void Warn(const char* env, const char* value) {
  fprintf(stderr, "Warning: I didn't understand %s=\"%s\"\n", env, value);
}

static Maybe<int> ParseInt(const char* str) {
  char* endp;
  int retval = strtol(str, &endp, 0);
  if (*endp == '\0') {
    return mozilla::Some(retval);
  }
  return mozilla::Nothing();
}

template <typename T>
T overrideDefault(const char* param, T dflt) {
  char* str = getenv(param);
  if (!str) {
    return dflt;
  }
  if constexpr (std::is_same_v<T, bool>) {
    if (strcmp(str, "true") == 0 || strcmp(str, "yes") == 0) {
      return true;
    }
    if (strcmp(str, "false") == 0 || strcmp(str, "no") == 0) {
      return false;
    }
    Warn(param, str);
  } else {
    Maybe<int> value = ParseInt(str);
    if (value.isSome()) {
      return value.ref();
    }
    Warn(param, str);
  }
  return dflt;
}

#define SET_DEFAULT(var, dflt) var = overrideDefault("JIT_OPTION_" #var, dflt)
DefaultJitOptions::DefaultJitOptions() {
  // Whether to perform expensive graph-consistency DEBUG-only assertions.
  // It can be useful to disable this to reduce DEBUG-compile time of large
  // wasm programs.
  SET_DEFAULT(checkGraphConsistency, true);

#ifdef CHECK_OSIPOINT_REGISTERS
  // Emit extra code to verify live regs at the start of a VM call
  // are not modified before its OsiPoint.
  SET_DEFAULT(checkOsiPointRegisters, false);
#endif

  // Whether to enable extra code to perform dynamic validation of
  // RangeAnalysis results.
  SET_DEFAULT(checkRangeAnalysis, false);

  // Toggles whether Alignment Mask Analysis is globally disabled.
  SET_DEFAULT(disableAma, false);

  // Toggles whether Effective Address Analysis is globally disabled.
  SET_DEFAULT(disableEaa, false);

  // Toggles whether Edge Case Analysis is gobally disabled.
  SET_DEFAULT(disableEdgeCaseAnalysis, false);

  // Toggle whether global value numbering is globally disabled.
  SET_DEFAULT(disableGvn, false);

  // Toggles whether inlining is globally disabled.
  SET_DEFAULT(disableInlining, false);

  // Toggles whether loop invariant code motion is globally disabled.
  SET_DEFAULT(disableLicm, false);

  // Toggle whether branch pruning is globally disabled.
  SET_DEFAULT(disablePruning, false);

  // Toggles whether instruction reordering is globally disabled.
  SET_DEFAULT(disableInstructionReordering, false);

  // Toggles whether Range Analysis is globally disabled.
  SET_DEFAULT(disableRangeAnalysis, false);

  // Toggles wheter Recover instructions is globally disabled.
  SET_DEFAULT(disableRecoverIns, false);

  // Toggle whether eager scalar replacement is globally disabled.
  SET_DEFAULT(disableScalarReplacement, false);

  // Toggles whether CacheIR stubs are used.
  SET_DEFAULT(disableCacheIR, false);

  // Toggles whether sink code motion is globally disabled.
  SET_DEFAULT(disableSink, true);

  // Toggles whether we verify that we don't recompile with the same CacheIR.
  SET_DEFAULT(disableBailoutLoopCheck, false);

  // Whether the Baseline Interpreter is enabled.
  SET_DEFAULT(baselineInterpreter, true);

  // Whether the Baseline JIT is enabled.
  SET_DEFAULT(baselineJit, true);

  // Whether the IonMonkey JIT is enabled.
  SET_DEFAULT(ion, true);

  // Warp compile Async functions
  SET_DEFAULT(warpAsync, true);

  // Warp compile Generator functions
  SET_DEFAULT(warpGenerator, true);

  // Whether the IonMonkey and Baseline JITs are enabled for Trusted Principals.
  // (Ignored if ion or baselineJit is set to true.)
  SET_DEFAULT(jitForTrustedPrincipals, false);

  // Whether the RegExp JIT is enabled.
  SET_DEFAULT(nativeRegExp, true);

  // Whether Warp should use ICs instead of transpiling Baseline CacheIR.
  SET_DEFAULT(forceInlineCaches, false);

  // Whether all ICs should be initialized as megamorphic ICs.
  SET_DEFAULT(forceMegamorphicICs, false);

  // Toggles whether large scripts are rejected.
  SET_DEFAULT(limitScriptSize, true);

  // Toggles whether functions may be entered at loop headers.
  SET_DEFAULT(osr, true);

  // Whether to enable extra code to perform dynamic validations.
  SET_DEFAULT(runExtraChecks, false);

  // How many invocations or loop iterations are needed before functions
  // enter the Baseline Interpreter.
  SET_DEFAULT(baselineInterpreterWarmUpThreshold, 10);

  // How many invocations or loop iterations are needed before functions
  // are compiled with the baseline compiler.
  // Duplicated in all.js - ensure both match.
  SET_DEFAULT(baselineJitWarmUpThreshold, 100);

  // How many invocations or loop iterations are needed before functions
  // are considered for trial inlining.
  SET_DEFAULT(trialInliningWarmUpThreshold, 500);

  // The initial warm-up count for ICScripts created by trial inlining.
  //
  // Note: the difference between trialInliningInitialWarmUpCount and
  // trialInliningWarmUpThreshold must be:
  //
  // * Small enough to allow inlining multiple levels deep before the outer
  //   script reaches its normalIonWarmUpThreshold.
  //
  // * Greater than inliningEntryThreshold or no scripts can be inlined.
  SET_DEFAULT(trialInliningInitialWarmUpCount, 250);

  // How many invocations or loop iterations are needed before functions
  // are compiled with the Ion compiler at OptimizationLevel::Normal.
  // Duplicated in all.js - ensure both match.
  SET_DEFAULT(normalIonWarmUpThreshold, 1500);

  // How many invocations are needed before regexps are compiled to
  // native code.
  SET_DEFAULT(regexpWarmUpThreshold, 10);

  // Number of exception bailouts (resuming into catch/finally block) before
  // we invalidate and forbid Ion compilation.
  SET_DEFAULT(exceptionBailoutThreshold, 10);

  // Number of bailouts without invalidation before we set
  // JSScript::hadFrequentBailouts and invalidate.
  // Duplicated in all.js - ensure both match.
  SET_DEFAULT(frequentBailoutThreshold, 10);

  // Whether to run all debug checks in debug builds.
  // Disabling might make it more enjoyable to run JS in debug builds.
  SET_DEFAULT(fullDebugChecks, true);

  // How many actual arguments are accepted on the C stack.
  SET_DEFAULT(maxStackArgs, 4096);

  // How many times we will try to enter a script via OSR before
  // invalidating the script.
  SET_DEFAULT(osrPcMismatchesBeforeRecompile, 6000);

  // The bytecode length limit for small function.
  SET_DEFAULT(smallFunctionMaxBytecodeLength, 130);

  // The minimum entry count for an IC stub before it can be trial-inlined.
  SET_DEFAULT(inliningEntryThreshold, 100);

  // An artificial testing limit for the maximum supported offset of
  // pc-relative jump and call instructions.
  SET_DEFAULT(jumpThreshold, UINT32_MAX);

  // Branch pruning heuristic is based on a scoring system, which is look at
  // different metrics and provide a score. The score is computed as a
  // projection where each factor defines the weight of each metric. Then this
  // score is compared against a threshold to prevent a branch from being
  // removed.
  SET_DEFAULT(branchPruningHitCountFactor, 1);
  SET_DEFAULT(branchPruningInstFactor, 10);
  SET_DEFAULT(branchPruningBlockSpanFactor, 100);
  SET_DEFAULT(branchPruningEffectfulInstFactor, 3500);
  SET_DEFAULT(branchPruningThreshold, 4000);

  // Limits on bytecode length and number of locals/arguments for Ion
  // compilation. There are different (lower) limits for when off-thread Ion
  // compilation isn't available.
  SET_DEFAULT(ionMaxScriptSize, 100 * 1000);
  SET_DEFAULT(ionMaxScriptSizeMainThread, 2 * 1000);
  SET_DEFAULT(ionMaxLocalsAndArgs, 10 * 1000);
  SET_DEFAULT(ionMaxLocalsAndArgsMainThread, 256);

  // Force the used register allocator instead of letting the optimization
  // pass decide.
  const char* forcedRegisterAllocatorEnv = "JIT_OPTION_forcedRegisterAllocator";
  if (const char* env = getenv(forcedRegisterAllocatorEnv)) {
    forcedRegisterAllocator = LookupRegisterAllocator(env);
    if (!forcedRegisterAllocator.isSome()) {
      Warn(forcedRegisterAllocatorEnv, env);
    }
  }

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  SET_DEFAULT(spectreIndexMasking, false);
  SET_DEFAULT(spectreObjectMitigations, false);
  SET_DEFAULT(spectreStringMitigations, false);
  SET_DEFAULT(spectreValueMasking, false);
  SET_DEFAULT(spectreJitToCxxCalls, false);
#else
  SET_DEFAULT(spectreIndexMasking, true);
  SET_DEFAULT(spectreObjectMitigations, true);
  SET_DEFAULT(spectreStringMitigations, true);
  SET_DEFAULT(spectreValueMasking, true);
  SET_DEFAULT(spectreJitToCxxCalls, true);
#endif

  // These are set to their actual values in InitializeJit.
  SET_DEFAULT(supportsFloatingPoint, false);
  SET_DEFAULT(supportsUnalignedAccesses, false);

  // Toggles the optimization whereby offsets are folded into loads and not
  // included in the bounds check.
  SET_DEFAULT(wasmFoldOffsets, true);

  // Controls whether two-tiered compilation should be requested when
  // compiling a new wasm module, independently of other heuristics, and
  // should be delayed to test both baseline and ion paths in compiled code,
  // as well as the transition from one tier to the other.
  SET_DEFAULT(wasmDelayTier2, false);

  // Until which wasm bytecode size should we accumulate functions, in order
  // to compile efficiently on helper threads. Baseline code compiles much
  // faster than Ion code so use scaled thresholds (see also bug 1320374).
  // Cranelift compiles at about half the speed of Ion, but is much more
  // affected by malloc/free costs, so set its threshold relatively high, in
  // order to reduce overall allocation costs.  See bug 1586791.
  SET_DEFAULT(wasmBatchBaselineThreshold, 10000);
  SET_DEFAULT(wasmBatchIonThreshold, 1100);
  SET_DEFAULT(wasmBatchCraneliftThreshold, 5000);

#ifdef JS_TRACE_LOGGING
  // Toggles whether the traceLogger should be on or off.  In either case,
  // some data structures will always be created and initialized such as
  // the traceLoggerState.  However, unless this option is set to true
  // the traceLogger will not be recording any events.
  SET_DEFAULT(enableTraceLogger, false);
#endif

  // Dumps a representation of parsed regexps to stderr
  SET_DEFAULT(traceRegExpParser, false);
  // Dumps the calls made to the regexp assembler to stderr
  SET_DEFAULT(traceRegExpAssembler, false);
  // Dumps the bytecodes interpreted by the regexp engine to stderr
  SET_DEFAULT(traceRegExpInterpreter, false);
  // Dumps the changes made by the regexp peephole optimizer to stderr
  SET_DEFAULT(traceRegExpPeephole, false);

  SET_DEFAULT(enableWasmJitExit, true);
  SET_DEFAULT(enableWasmJitEntry, true);
  SET_DEFAULT(enableWasmIonFastCalls, true);
#ifdef WASM_CODEGEN_DEBUG
  SET_DEFAULT(enableWasmImportCallSpew, false);
  SET_DEFAULT(enableWasmFuncCallSpew, false);
#endif
}

bool DefaultJitOptions::isSmallFunction(JSScript* script) const {
  return script->length() <= smallFunctionMaxBytecodeLength;
}

void DefaultJitOptions::enableGvn(bool enable) { disableGvn = !enable; }

void DefaultJitOptions::setEagerBaselineCompilation() {
  baselineInterpreterWarmUpThreshold = 0;
  baselineJitWarmUpThreshold = 0;
  regexpWarmUpThreshold = 0;
}

void DefaultJitOptions::setEagerIonCompilation() {
  setEagerBaselineCompilation();
  normalIonWarmUpThreshold = 0;
}

void DefaultJitOptions::setFastWarmUp() {
  baselineInterpreterWarmUpThreshold = 4;
  baselineJitWarmUpThreshold = 10;
  trialInliningWarmUpThreshold = 14;
  trialInliningInitialWarmUpCount = 12;
  normalIonWarmUpThreshold = 30;

  inliningEntryThreshold = 2;
  smallFunctionMaxBytecodeLength = 2000;
}

void DefaultJitOptions::setNormalIonWarmUpThreshold(uint32_t warmUpThreshold) {
  normalIonWarmUpThreshold = warmUpThreshold;
}

void DefaultJitOptions::resetNormalIonWarmUpThreshold() {
  jit::DefaultJitOptions defaultValues;
  setNormalIonWarmUpThreshold(defaultValues.normalIonWarmUpThreshold);
}

}  // namespace jit
}  // namespace js
