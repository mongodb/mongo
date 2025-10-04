/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitOptions.h"

#include <cstdlib>
#include <type_traits>

#include "vm/JSScript.h"

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

  // Toggles whether the iterator indices optimization is globally disabled.
  SET_DEFAULT(disableIteratorIndices, false);

  // Toggles whether instruction reordering is globally disabled.
  SET_DEFAULT(disableInstructionReordering, false);

  // Toggles whether atomizing loads used as property keys is globally disabled.
  SET_DEFAULT(disableMarkLoadsUsedAsPropertyKeys, false);

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

  // Toggles whether redundant shape guard elimination is globally disabled.
  SET_DEFAULT(disableRedundantShapeGuards, false);

  // Toggles whether redundant GC barrier elimination is globally disabled.
  SET_DEFAULT(disableRedundantGCBarriers, false);

  // Toggles whether we verify that we don't recompile with the same CacheIR.
  SET_DEFAULT(disableBailoutLoopCheck, false);

  // Whether the Baseline Interpreter is enabled.
  SET_DEFAULT(baselineInterpreter, true);

#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  // Whether the Portable Baseline Interpreter is enabled.
  SET_DEFAULT(portableBaselineInterpreter, false);
#endif

#ifdef ENABLE_PORTABLE_BASELINE_INTERP_FORCE
  SET_DEFAULT(portableBaselineInterpreter, true);
  SET_DEFAULT(portableBaselineInterpreterWarmUpThreshold, 0);
#endif

  // emitInterpreterEntryTrampoline and enableICFramePointers are used in
  // combination with perf jitdump profiling.  The first will enable
  // trampolines for interpreter and baseline interpreter frames to
  // identify which function is being executed, and the latter enables
  // frame pointers for IC stubs.  They are both enabled by default
  // when the |IONPERF| environment variable is set.
  bool perfEnabled = !!getenv("IONPERF");
  SET_DEFAULT(emitInterpreterEntryTrampoline, perfEnabled);
  SET_DEFAULT(enableICFramePointers, perfEnabled);

  // Whether the Baseline JIT is enabled.
  SET_DEFAULT(baselineJit, true);

  // Whether the IonMonkey JIT is enabled.
  SET_DEFAULT(ion, true);

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

  // Whether the JIT backend (used by JITs, Wasm, Baseline Interpreter) has been
  // disabled for this process. See JS::DisableJitBackend.
  SET_DEFAULT(disableJitBackend, false);

  // Whether to enable extra code to perform dynamic validations.
  SET_DEFAULT(runExtraChecks, false);

  // How many invocations or loop iterations are needed before functions
  // enter the Baseline Interpreter.
  SET_DEFAULT(baselineInterpreterWarmUpThreshold, 10);

#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  // How many invocations are needed before functions enter the
  // Portable Baseline Interpreter.
  SET_DEFAULT(portableBaselineInterpreterWarmUpThreshold, 10);
#endif

  // How many invocations or loop iterations are needed before functions
  // are compiled with the baseline compiler.
  // Duplicated in all.js - ensure both match.
  SET_DEFAULT(baselineJitWarmUpThreshold, 100);

  // Disable eager baseline jit hints
  SET_DEFAULT(disableJitHints, false);

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
  SET_DEFAULT(maxStackArgs, 20'000);

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

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_RISCV64)
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
  SET_DEFAULT(spectreJitToCxxCalls, false);
#endif

  // Whether the W^X policy is enforced to mark JIT code pages as either
  // writable or executable but never both at the same time. On Apple Silicon
  // this must always be false because we use pthread_jit_write_protect_np.
#ifdef JS_USE_APPLE_FAST_WX
  SET_DEFAULT(writeProtectCode, false);
#else
  SET_DEFAULT(writeProtectCode, true);
#endif

  // This is set to its actual value in InitializeJit.
  SET_DEFAULT(supportsUnalignedAccesses, false);

  // To access local (non-argument) slots, it's more efficient to use the frame
  // pointer (FP) instead of the stack pointer (SP) as base register on x86 and
  // x64 (because instructions are one byte shorter, for example).
  //
  // However, because this requires a negative offset from FP, on ARM64 it can
  // be more efficient to use SP-relative addresses for larger stack frames
  // because the range for load/store immediate offsets is [-256, 4095] and
  // offsets outside this range will require an extra instruction.
  //
  // We default to FP-relative addresses on x86/x64 and SP-relative on other
  // platforms, but to improve fuzzing we allow changing this in the shell:
  //
  //   setJitCompilerOption("base-reg-for-locals", N); // 0 for SP, 1 for FP
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  baseRegForLocals = BaseRegForAddress::FP;
#else
  baseRegForLocals = BaseRegForAddress::SP;
#endif

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
  SET_DEFAULT(wasmBatchBaselineThreshold, 10000);
  SET_DEFAULT(wasmBatchIonThreshold, 1100);

  // Controls how much assertion checking code is emitted
  SET_DEFAULT(lessDebugCode, false);

  SET_DEFAULT(onlyInlineSelfHosted, false);

  SET_DEFAULT(enableWasmJitExit, true);
  SET_DEFAULT(enableWasmJitEntry, true);
  SET_DEFAULT(enableWasmIonFastCalls, true);
#ifdef WASM_CODEGEN_DEBUG
  SET_DEFAULT(enableWasmImportCallSpew, false);
  SET_DEFAULT(enableWasmFuncCallSpew, false);
#endif

  // This is used to control whether regexps tier up from interpreted to
  // compiled. We control this with --no-native-regexp and
  // --regexp-warmup-threshold.
  SET_DEFAULT(regexp_tier_up, true);

  // Dumps a representation of parsed regexps to stderr
  SET_DEFAULT(trace_regexp_parser, false);
  // Dumps the calls made to the regexp assembler to stderr
  SET_DEFAULT(trace_regexp_assembler, false);
  // Dumps the bytecodes interpreted by the regexp engine to stderr
  SET_DEFAULT(trace_regexp_bytecodes, false);
  // Dumps the changes made by the regexp peephole optimizer to stderr
  SET_DEFAULT(trace_regexp_peephole_optimization, false);

  // ***** Irregexp shim flags *****

  // Whether the stage 3 regexp modifiers proposal is enabled.
  SET_DEFAULT(js_regexp_modifiers, false);
  // Whether the stage 3 duplicate named capture groups proposal is enabled.
  SET_DEFAULT(js_regexp_duplicate_named_groups, false);
  // V8 uses this for differential fuzzing to handle stack overflows.
  // We address the same problem in StackLimitCheck::HasOverflowed.
  SET_DEFAULT(correctness_fuzzer_suppressions, false);
  // Instead of using a flag for this, we provide an implementation of
  // CanReadUnaligned in SMRegExpMacroAssembler.
  SET_DEFAULT(enable_regexp_unaligned_accesses, false);
  // This is used to guard an old prototype implementation of possessive
  // quantifiers, which never got past the point of adding parser support.
  SET_DEFAULT(regexp_possessive_quantifier, false);
  // These affect the default level of optimization. We can still turn
  // optimization off on a case-by-case basis in CompilePattern - for
  // example, if a regexp is too long - so we might as well turn these
  // flags on unconditionally.
  SET_DEFAULT(regexp_optimization, true);
#if MOZ_BIG_ENDIAN()
  // peephole optimization not supported on big endian
  SET_DEFAULT(regexp_peephole_optimization, false);
#else
  SET_DEFAULT(regexp_peephole_optimization, true);
#endif
}

bool DefaultJitOptions::isSmallFunction(JSScript* script) const {
  return script->length() <= smallFunctionMaxBytecodeLength;
}

void DefaultJitOptions::enableGvn(bool enable) { disableGvn = !enable; }

#ifdef ENABLE_PORTABLE_BASELINE_INTERP
void DefaultJitOptions::setEagerPortableBaselineInterpreter() {
  portableBaselineInterpreterWarmUpThreshold = 0;
}
#endif

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

void DefaultJitOptions::maybeSetWriteProtectCode(bool val) {
#ifdef JS_USE_APPLE_FAST_WX
  // On Apple Silicon we always use pthread_jit_write_protect_np, or
  // be_memory_inline_jit_restrict_*.
  MOZ_ASSERT(!writeProtectCode);
#else
  writeProtectCode = val;
#endif
}

}  // namespace jit
}  // namespace js
