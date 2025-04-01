/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmCompile.h"

#include "mozilla/Maybe.h"

#include <algorithm>

#include "js/Equality.h"
#include "js/ForOfIterator.h"
#include "js/PropertyAndElement.h"

#ifndef __wasi__
#  include "jit/ProcessExecutableMemory.h"
#endif

#include "jit/FlushICache.h"
#include "jit/JitOptions.h"
#include "util/Text.h"
#include "vm/HelperThreads.h"
#include "vm/JSAtomState.h"
#include "vm/Realm.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmProcess.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

uint32_t wasm::ObservedCPUFeatures() {
  enum Arch {
    X86 = 0x1,
    X64 = 0x2,
    ARM = 0x3,
    MIPS = 0x4,
    MIPS64 = 0x5,
    ARM64 = 0x6,
    LOONG64 = 0x7,
    RISCV64 = 0x8,
    ARCH_BITS = 3
  };

#if defined(JS_CODEGEN_X86)
  MOZ_ASSERT(uint32_t(jit::CPUInfo::GetFingerprint()) <=
             (UINT32_MAX >> ARCH_BITS));
  return X86 | (uint32_t(jit::CPUInfo::GetFingerprint()) << ARCH_BITS);
#elif defined(JS_CODEGEN_X64)
  MOZ_ASSERT(uint32_t(jit::CPUInfo::GetFingerprint()) <=
             (UINT32_MAX >> ARCH_BITS));
  return X64 | (uint32_t(jit::CPUInfo::GetFingerprint()) << ARCH_BITS);
#elif defined(JS_CODEGEN_ARM)
  MOZ_ASSERT(jit::GetARMFlags() <= (UINT32_MAX >> ARCH_BITS));
  return ARM | (jit::GetARMFlags() << ARCH_BITS);
#elif defined(JS_CODEGEN_ARM64)
  MOZ_ASSERT(jit::GetARM64Flags() <= (UINT32_MAX >> ARCH_BITS));
  return ARM64 | (jit::GetARM64Flags() << ARCH_BITS);
#elif defined(JS_CODEGEN_MIPS64)
  MOZ_ASSERT(jit::GetMIPSFlags() <= (UINT32_MAX >> ARCH_BITS));
  return MIPS64 | (jit::GetMIPSFlags() << ARCH_BITS);
#elif defined(JS_CODEGEN_LOONG64)
  MOZ_ASSERT(jit::GetLOONG64Flags() <= (UINT32_MAX >> ARCH_BITS));
  return LOONG64 | (jit::GetLOONG64Flags() << ARCH_BITS);
#elif defined(JS_CODEGEN_RISCV64)
  MOZ_ASSERT(jit::GetRISCV64Flags() <= (UINT32_MAX >> ARCH_BITS));
  return RISCV64 | (jit::GetRISCV64Flags() << ARCH_BITS);
#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)
  return 0;
#else
#  error "unknown architecture"
#endif
}

bool FeatureOptions::init(JSContext* cx, HandleValue val) {
  if (val.isNullOrUndefined()) {
    return true;
  }

#ifdef ENABLE_WASM_JS_STRING_BUILTINS
  if (JSStringBuiltinsAvailable(cx)) {
    if (!val.isObject()) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_BAD_COMPILE_OPTIONS);
      return false;
    }
    RootedObject obj(cx, &val.toObject());

    // Get the `builtins` iterable
    RootedValue builtins(cx);
    if (!JS_GetProperty(cx, obj, "builtins", &builtins)) {
      return false;
    }

    JS::ForOfIterator iterator(cx);

    if (!iterator.init(builtins, JS::ForOfIterator::ThrowOnNonIterable)) {
      return false;
    }

    RootedValue jsStringModule(cx, StringValue(cx->names().jsStringModule));
    RootedValue nextBuiltin(cx);
    while (true) {
      bool done;
      if (!iterator.next(&nextBuiltin, &done)) {
        return false;
      }
      if (done) {
        break;
      }

      bool jsStringBuiltins;
      if (!JS::LooselyEqual(cx, nextBuiltin, jsStringModule,
                            &jsStringBuiltins)) {
        return false;
      }

      if (!jsStringBuiltins) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_UNKNOWN_BUILTIN);
        return false;
      }

      if (this->jsStringBuiltins && jsStringBuiltins) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_DUPLICATE_BUILTIN);
        return false;
      }
      this->jsStringBuiltins = jsStringBuiltins;
    }
  }
#endif

  return true;
}

FeatureArgs FeatureArgs::build(JSContext* cx, const FeatureOptions& options) {
  FeatureArgs features;

#define WASM_FEATURE(NAME, LOWER_NAME, ...) \
  features.LOWER_NAME = wasm::NAME##Available(cx);
  JS_FOR_WASM_FEATURES(WASM_FEATURE);
#undef WASM_FEATURE

  features.sharedMemory =
      wasm::ThreadsAvailable(cx) ? Shareable::True : Shareable::False;

  features.simd = jit::JitSupportsWasmSimd();
  features.isBuiltinModule = options.isBuiltinModule;
  if (features.jsStringBuiltins) {
    features.builtinModules.jsString = options.jsStringBuiltins;
  }
#ifdef ENABLE_WASM_GC
  if (options.requireGC) {
    features.gc = true;
  }
#endif
#ifdef ENABLE_WASM_TAIL_CALLS
  if (options.requireTailCalls) {
    features.tailCalls = true;
  }
#endif

  return features;
}

SharedCompileArgs CompileArgs::build(JSContext* cx,
                                     ScriptedCaller&& scriptedCaller,
                                     const FeatureOptions& options,
                                     CompileArgsError* error) {
  bool baseline = BaselineAvailable(cx);
  bool ion = IonAvailable(cx);

  // Debug information such as source view or debug traps will require
  // additional memory and permanently stay in baseline code, so we try to
  // only enable it when a developer actually cares: when the debugger tab
  // is open.
  bool debug = cx->realm() && cx->realm()->debuggerObservesWasm();

  bool forceTiering =
      cx->options().testWasmAwaitTier2() || JitOptions.wasmDelayTier2;

  // The <Compiler>Available() predicates should ensure no failure here, but
  // when we're fuzzing we allow inconsistent switches and the check may thus
  // fail.  Let it go to a run-time error instead of crashing.
  if (debug && ion) {
    *error = CompileArgsError::NoCompiler;
    return nullptr;
  }

  if (forceTiering && !(baseline && ion)) {
    // This can happen only in testing, and in this case we don't have a
    // proper way to signal the error, so just silently override the default,
    // instead of adding a skip-if directive to every test using debug/gc.
    forceTiering = false;
  }

  if (!(baseline || ion)) {
    *error = CompileArgsError::NoCompiler;
    return nullptr;
  }

  CompileArgs* target = cx->new_<CompileArgs>(std::move(scriptedCaller));
  if (!target) {
    *error = CompileArgsError::OutOfMemory;
    return nullptr;
  }

  target->baselineEnabled = baseline;
  target->ionEnabled = ion;
  target->debugEnabled = debug;
  target->forceTiering = forceTiering;
  target->features = FeatureArgs::build(cx, options);

  return target;
}

void wasm::SetUseCountersForFeatureUsage(JSContext* cx, JSObject* object,
                                         FeatureUsage usage) {
  if (usage & FeatureUsage::LegacyExceptions) {
    cx->runtime()->setUseCounter(object, JSUseCounter::WASM_LEGACY_EXCEPTIONS);
  }
}

SharedCompileArgs CompileArgs::buildForAsmJS(ScriptedCaller&& scriptedCaller) {
  CompileArgs* target = js_new<CompileArgs>(std::move(scriptedCaller));
  if (!target) {
    return nullptr;
  }

  // AsmJS is deprecated and doesn't have mechanisms for experimental features,
  // so we don't need to initialize the FeatureArgs. It also only targets the
  // Ion backend and does not need WASM debug support since it is de-optimized
  // to JS in that case.
  target->ionEnabled = true;
  target->debugEnabled = false;

  return target;
}

SharedCompileArgs CompileArgs::buildAndReport(JSContext* cx,
                                              ScriptedCaller&& scriptedCaller,
                                              const FeatureOptions& options,
                                              bool reportOOM) {
  CompileArgsError error;
  SharedCompileArgs args =
      CompileArgs::build(cx, std::move(scriptedCaller), options, &error);
  if (args) {
    Log(cx, "available wasm compilers: tier1=%s tier2=%s",
        args->baselineEnabled ? "baseline" : "none",
        args->ionEnabled ? "ion" : "none");
    return args;
  }

  switch (error) {
    case CompileArgsError::NoCompiler: {
      JS_ReportErrorASCII(cx, "no WebAssembly compiler available");
      break;
    }
    case CompileArgsError::OutOfMemory: {
      // Most callers are required to return 'false' without reporting an OOM,
      // so we make reporting it optional here.
      if (reportOOM) {
        ReportOutOfMemory(cx);
      }
      break;
    }
  }
  return nullptr;
}

/*
 * [SMDOC] Tiered wasm compilation.
 *
 * "Tiered compilation" refers to the mechanism where we first compile the code
 * with a fast non-optimizing compiler so that we can start running the code
 * quickly, while in the background recompiling the code with the slower
 * optimizing compiler.  Code created by baseline is called "tier-1"; code
 * created by the optimizing compiler is called "tier-2".  When the tier-2 code
 * is ready, we "tier up" the code by creating paths from tier-1 code into their
 * tier-2 counterparts; this patching is performed as the program is running.
 *
 * ## Selecting the compilation mode
 *
 * When wasm bytecode arrives, we choose the compilation strategy based on
 * switches and on aspects of the code and the hardware.  If switches allow
 * tiered compilation to happen (the normal case), the following logic applies.
 *
 * If the code is sufficiently large that tiered compilation would be beneficial
 * but not so large that it might blow our compiled code budget and make
 * compilation fail, we choose tiered compilation.  Otherwise we go straight to
 * optimized code.
 *
 * The expected benefit of tiering is computed by TieringBeneficial(), below,
 * based on various estimated parameters of the hardware: ratios of object code
 * to byte code, speed of the system, number of cores.
 *
 * ## Mechanics of tiering up; patching
 *
 * Every time control enters a tier-1 function, the function prologue loads its
 * tiering pointer from the tiering jump table (see JumpTable in WasmCode.h) and
 * jumps to it.
 *
 * Initially, an entry in the tiering table points to the instruction inside the
 * tier-1 function that follows the jump instruction (hence the jump is an
 * expensive nop).  When the tier-2 compiler is finished, the table is patched
 * racily to point into the tier-2 function at the correct prologue location
 * (see loop near the end of Module::finishTier2()).  As tier-2 compilation is
 * performed at most once per Module, there is at most one such racy overwrite
 * per table element during the lifetime of the Module.
 *
 * The effect of the patching is to cause the tier-1 function to jump to its
 * tier-2 counterpart whenever the tier-1 function is called subsequently.  That
 * is, tier-1 code performs standard frame setup on behalf of whatever code it
 * jumps to, and the target code (tier-1 or tier-2) allocates its own frame in
 * whatever way it wants.
 *
 * The racy writing means that it is often nondeterministic whether tier-1 or
 * tier-2 code is reached by any call during the tiering-up process; if F calls
 * A and B in that order, it may reach tier-2 code for A and tier-1 code for B.
 * If F is running concurrently on threads T1 and T2, T1 and T2 may see code
 * from different tiers for either function.
 *
 * Note, tiering up also requires upgrading the jit-entry stubs so that they
 * reference tier-2 code.  The mechanics of this upgrading are described at
 * WasmInstanceObject::getExportedFunction().
 *
 * ## Current limitations of tiering
 *
 * Tiering is not always seamless.  Partly, it is possible for a program to get
 * stuck in tier-1 code.  Partly, a function that has tiered up continues to
 * force execution to go via tier-1 code to reach tier-2 code, paying for an
 * additional jump and a slightly less optimized prologue than tier-2 code could
 * have had on its own.
 *
 * Known tiering limitiations:
 *
 * - We can tier up only at function boundaries.  If a tier-1 function has a
 *   long-running loop it will not tier up until it returns to its caller.  If
 *   this loop never exits (a runloop in a worker, for example) then the
 *   function will never tier up.
 *
 *   To do better, we need OSR.
 *
 * - Wasm Table entries are never patched during tier-up.  A Table of funcref
 *   holds not a JSFunction pointer, but a (code*,instance*) pair of pointers.
 * When a table.set operation is performed, the JSFunction value is decomposed
 * and its code and instance pointers are stored in the table; subsequently,
 * when a table.get operation is performed, the JSFunction value is
 * reconstituted from its code pointer using fairly elaborate machinery.  (The
 * mechanics are the same also for the reflected JS operations on a
 * WebAssembly.Table.  For everything, see WasmTable.{cpp,h}.)  The code pointer
 * in the Table will always be the code pointer belonging to the best tier that
 * was active at the time when that function was stored in that Table slot; in
 * many cases, it will be tier-1 code.  As a consequence, a call through a table
 * will first enter tier-1 code and then jump to tier-2 code.
 *
 *   To do better, we must update all the tables in the system when an instance
 *   tiers up.  This is expected to be very hard.
 *
 * - Imported Wasm functions are never patched during tier-up.  Imports are held
 *   in FuncImportInstanceData values in the instance, and for a wasm
 *   callee, what's stored is the raw code pointer into the best tier of the
 *   callee that was active at the time the import was resolved.  That could be
 *   baseline code, and if it is, the situation is as for Table entries: a call
 *   to an import will always go via that import's tier-1 code, which will tier
 * up with an indirect jump.
 *
 *   To do better, we must update all the import tables in the system that
 *   import functions from instances whose modules have tiered up.  This is
 *   expected to be hard.
 */

// Classify the current system as one of a set of recognizable classes.  This
// really needs to get our tier-1 systems right.
//
// TODO: We don't yet have a good measure of how fast a system is.  We
// distinguish between mobile and desktop because these are very different kinds
// of systems, but we could further distinguish between low / medium / high end
// within those major classes.  If we do so, then constants below would be
// provided for each (class, architecture, system-tier) combination, not just
// (class, architecture) as now.
//
// CPU clock speed is not by itself a good predictor of system performance, as
// there are high-performance systems with slow clocks (recent Intel) and
// low-performance systems with fast clocks (older AMD).  We can also use
// physical memory, core configuration, OS details, CPU class and family, and
// CPU manufacturer to disambiguate.

enum class SystemClass {
  DesktopX86,
  DesktopX64,
  DesktopUnknown32,
  DesktopUnknown64,
  MobileX86,
  MobileArm32,
  MobileArm64,
  MobileUnknown32,
  MobileUnknown64
};

static SystemClass ClassifySystem() {
  bool isDesktop;

#if defined(ANDROID) || defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
  isDesktop = false;
#else
  isDesktop = true;
#endif

  if (isDesktop) {
#if defined(JS_CODEGEN_X64)
    return SystemClass::DesktopX64;
#elif defined(JS_CODEGEN_X86)
    return SystemClass::DesktopX86;
#elif defined(JS_64BIT)
    return SystemClass::DesktopUnknown64;
#else
    return SystemClass::DesktopUnknown32;
#endif
  } else {
#if defined(JS_CODEGEN_X86)
    return SystemClass::MobileX86;
#elif defined(JS_CODEGEN_ARM)
    return SystemClass::MobileArm32;
#elif defined(JS_CODEGEN_ARM64)
    return SystemClass::MobileArm64;
#elif defined(JS_64BIT)
    return SystemClass::MobileUnknown64;
#else
    return SystemClass::MobileUnknown32;
#endif
  }
}

// Code sizes in machine code bytes per bytecode byte, again empirical except
// where marked.
//
// The Ion estimate for ARM64 is the measured Baseline value scaled by a
// plausible factor for optimized code.

static const double x64Tox86Inflation = 1.25;

static const double x64IonBytesPerBytecode = 2.45;
static const double x86IonBytesPerBytecode =
    x64IonBytesPerBytecode * x64Tox86Inflation;
static const double arm32IonBytesPerBytecode = 3.3;
static const double arm64IonBytesPerBytecode = 3.0 / 1.4;  // Estimate

static const double x64BaselineBytesPerBytecode = x64IonBytesPerBytecode * 1.43;
static const double x86BaselineBytesPerBytecode =
    x64BaselineBytesPerBytecode * x64Tox86Inflation;
static const double arm32BaselineBytesPerBytecode =
    arm32IonBytesPerBytecode * 1.39;
static const double arm64BaselineBytesPerBytecode = 3.0;

static double OptimizedBytesPerBytecode(SystemClass cls) {
  switch (cls) {
    case SystemClass::DesktopX86:
    case SystemClass::MobileX86:
    case SystemClass::DesktopUnknown32:
      return x86IonBytesPerBytecode;
    case SystemClass::DesktopX64:
    case SystemClass::DesktopUnknown64:
      return x64IonBytesPerBytecode;
    case SystemClass::MobileArm32:
    case SystemClass::MobileUnknown32:
      return arm32IonBytesPerBytecode;
    case SystemClass::MobileArm64:
    case SystemClass::MobileUnknown64:
      return arm64IonBytesPerBytecode;
    default:
      MOZ_CRASH();
  }
}

static double BaselineBytesPerBytecode(SystemClass cls) {
  switch (cls) {
    case SystemClass::DesktopX86:
    case SystemClass::MobileX86:
    case SystemClass::DesktopUnknown32:
      return x86BaselineBytesPerBytecode;
    case SystemClass::DesktopX64:
    case SystemClass::DesktopUnknown64:
      return x64BaselineBytesPerBytecode;
    case SystemClass::MobileArm32:
    case SystemClass::MobileUnknown32:
      return arm32BaselineBytesPerBytecode;
    case SystemClass::MobileArm64:
    case SystemClass::MobileUnknown64:
      return arm64BaselineBytesPerBytecode;
    default:
      MOZ_CRASH();
  }
}

double wasm::EstimateCompiledCodeSize(Tier tier, size_t bytecodeSize) {
  SystemClass cls = ClassifySystem();
  switch (tier) {
    case Tier::Baseline:
      return double(bytecodeSize) * BaselineBytesPerBytecode(cls);
    case Tier::Optimized:
      return double(bytecodeSize) * OptimizedBytesPerBytecode(cls);
  }
  MOZ_CRASH("bad tier");
}

// If parallel Ion compilation is going to take longer than this, we should
// tier.

static const double tierCutoffMs = 10;

// Compilation rate values are empirical except when noted, the reference
// systems are:
//
// Late-2013 MacBook Pro (2.6GHz 4 x hyperthreaded Haswell, Mac OS X)
// Late-2015 Nexus 5X (1.4GHz 4 x Cortex-A53 + 1.8GHz 2 x Cortex-A57, Android)
// Ca-2016 SoftIron Overdrive 1000 (1.7GHz 4 x Cortex-A57, Fedora)
//
// The rates are always per core.
//
// The estimate for ARM64 is the Baseline compilation rate on the SoftIron
// (because we have no Ion yet), divided by 5 to estimate Ion compile rate and
// then divided by 2 to make it more reasonable for consumer ARM64 systems.

static const double x64IonBytecodesPerMs = 2100;
static const double x86IonBytecodesPerMs = 1500;
static const double arm32IonBytecodesPerMs = 450;
static const double arm64IonBytecodesPerMs = 750;  // Estimate

// Tiering cutoff values: if code section sizes are below these values (when
// divided by the effective number of cores) we do not tier, because we guess
// that parallel Ion compilation will be fast enough.

static const double x64DesktopTierCutoff = x64IonBytecodesPerMs * tierCutoffMs;
static const double x86DesktopTierCutoff = x86IonBytecodesPerMs * tierCutoffMs;
static const double x86MobileTierCutoff = x86DesktopTierCutoff / 2;  // Guess
static const double arm32MobileTierCutoff =
    arm32IonBytecodesPerMs * tierCutoffMs;
static const double arm64MobileTierCutoff =
    arm64IonBytecodesPerMs * tierCutoffMs;

static double CodesizeCutoff(SystemClass cls) {
  switch (cls) {
    case SystemClass::DesktopX86:
    case SystemClass::DesktopUnknown32:
      return x86DesktopTierCutoff;
    case SystemClass::DesktopX64:
    case SystemClass::DesktopUnknown64:
      return x64DesktopTierCutoff;
    case SystemClass::MobileX86:
      return x86MobileTierCutoff;
    case SystemClass::MobileArm32:
    case SystemClass::MobileUnknown32:
      return arm32MobileTierCutoff;
    case SystemClass::MobileArm64:
    case SystemClass::MobileUnknown64:
      return arm64MobileTierCutoff;
    default:
      MOZ_CRASH();
  }
}

// As the number of cores grows the effectiveness of each core dwindles (on the
// systems we care about for SpiderMonkey).
//
// The data are empirical, computed from the observed compilation time of the
// Tanks demo code on a variable number of cores.
//
// The heuristic may fail on NUMA systems where the core count is high but the
// performance increase is nil or negative once the program moves beyond one
// socket.  However, few browser users have such systems.

static double EffectiveCores(uint32_t cores) {
  if (cores <= 3) {
    return pow(cores, 0.9);
  }
  return pow(cores, 0.75);
}

#ifndef JS_64BIT
// Don't tier if tiering will fill code memory to more to more than this
// fraction.

static const double spaceCutoffPct = 0.9;
#endif

// Figure out whether we should use tiered compilation or not.
static bool TieringBeneficial(uint32_t codeSize) {
  uint32_t cpuCount = GetHelperThreadCPUCount();
  MOZ_ASSERT(cpuCount > 0);

  // It's mostly sensible not to background compile when there's only one
  // hardware thread as we want foreground computation to have access to that.
  // However, if wasm background compilation helper threads can be given lower
  // priority then background compilation on single-core systems still makes
  // some kind of sense.  That said, this is a non-issue: as of September 2017
  // 1-core was down to 3.5% of our population and falling.

  if (cpuCount == 1) {
    return false;
  }

  // Compute the max number of threads available to do actual background
  // compilation work.

  uint32_t workers = GetMaxWasmCompilationThreads();

  // The number of cores we will use is bounded both by the CPU count and the
  // worker count, since the worker count already takes this into account.

  uint32_t cores = workers;

  SystemClass cls = ClassifySystem();

  // Ion compilation on available cores must take long enough to be worth the
  // bother.

  double cutoffSize = CodesizeCutoff(cls);
  double effectiveCores = EffectiveCores(cores);

  if ((codeSize / effectiveCores) < cutoffSize) {
    return false;
  }

  // Do not implement a size cutoff for 64-bit systems since the code size
  // budget for 64 bit is so large that it will hardly ever be an issue.
  // (Also the cutoff percentage might be different on 64-bit.)

#ifndef JS_64BIT
  // If the amount of executable code for baseline compilation jeopardizes the
  // availability of executable memory for ion code then do not tier, for now.
  //
  // TODO: For now we consider this module in isolation.  We should really
  // worry about what else is going on in this process and might be filling up
  // the code memory.  It's like we need some kind of code memory reservation
  // system or JIT compilation for large modules.

  double ionRatio = OptimizedBytesPerBytecode(cls);
  double baselineRatio = BaselineBytesPerBytecode(cls);
  double needMemory = codeSize * (ionRatio + baselineRatio);
  double availMemory = LikelyAvailableExecutableMemory();
  double cutoff = spaceCutoffPct * MaxCodeBytesPerProcess;

  // If the sum of baseline and ion code makes us exceeds some set percentage
  // of the executable memory then disable tiering.

  if ((MaxCodeBytesPerProcess - availMemory) + needMemory > cutoff) {
    return false;
  }
#endif

  return true;
}

// Ensure that we have the non-compiler requirements to tier safely.
static bool PlatformCanTier() {
  return CanUseExtraThreads() && jit::CanFlushExecutionContextForAllThreads();
}

CompilerEnvironment::CompilerEnvironment(const CompileArgs& args)
    : state_(InitialWithArgs), args_(&args) {}

CompilerEnvironment::CompilerEnvironment(CompileMode mode, Tier tier,
                                         DebugEnabled debugEnabled)
    : state_(InitialWithModeTierDebug),
      mode_(mode),
      tier_(tier),
      debug_(debugEnabled) {}

void CompilerEnvironment::computeParameters() {
  MOZ_ASSERT(state_ == InitialWithModeTierDebug);

  state_ = Computed;
}

void CompilerEnvironment::computeParameters(Decoder& d) {
  MOZ_ASSERT(!isComputed());

  if (state_ == InitialWithModeTierDebug) {
    computeParameters();
    return;
  }

  bool baselineEnabled = args_->baselineEnabled;
  bool ionEnabled = args_->ionEnabled;
  bool debugEnabled = args_->debugEnabled;
  bool forceTiering = args_->forceTiering;

  bool hasSecondTier = ionEnabled;
  MOZ_ASSERT_IF(debugEnabled, baselineEnabled);
  MOZ_ASSERT_IF(forceTiering, baselineEnabled && hasSecondTier);

  // Various constraints in various places should prevent failure here.
  MOZ_RELEASE_ASSERT(baselineEnabled || ionEnabled);

  uint32_t codeSectionSize = 0;

  SectionRange range;
  if (StartsCodeSection(d.begin(), d.end(), &range)) {
    codeSectionSize = range.size;
  }

  if (baselineEnabled && hasSecondTier &&
      (TieringBeneficial(codeSectionSize) || forceTiering) &&
      PlatformCanTier()) {
    mode_ = CompileMode::Tier1;
    tier_ = Tier::Baseline;
  } else {
    mode_ = CompileMode::Once;
    tier_ = hasSecondTier ? Tier::Optimized : Tier::Baseline;
  }

  debug_ = debugEnabled ? DebugEnabled::True : DebugEnabled::False;

  state_ = Computed;
}

template <class DecoderT, class ModuleGeneratorT>
static bool DecodeFunctionBody(DecoderT& d, ModuleGeneratorT& mg,
                               uint32_t funcIndex) {
  uint32_t bodySize;
  if (!d.readVarU32(&bodySize)) {
    return d.fail("expected number of function body bytes");
  }

  if (bodySize > MaxFunctionBytes) {
    return d.fail("function body too big");
  }

  const size_t offsetInModule = d.currentOffset();

  // Skip over the function body; it will be validated by the compilation
  // thread.
  const uint8_t* bodyBegin;
  if (!d.readBytes(bodySize, &bodyBegin)) {
    return d.fail("function body length too big");
  }

  return mg.compileFuncDef(funcIndex, offsetInModule, bodyBegin,
                           bodyBegin + bodySize);
}

template <class DecoderT, class ModuleGeneratorT>
static bool DecodeCodeSection(const ModuleEnvironment& env, DecoderT& d,
                              ModuleGeneratorT& mg) {
  if (!env.codeSection) {
    if (env.numFuncDefs() != 0) {
      return d.fail("expected code section");
    }

    return mg.finishFuncDefs();
  }

  uint32_t numFuncDefs;
  if (!d.readVarU32(&numFuncDefs)) {
    return d.fail("expected function body count");
  }

  if (numFuncDefs != env.numFuncDefs()) {
    return d.fail(
        "function body count does not match function signature count");
  }

  for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncDefs; funcDefIndex++) {
    if (!DecodeFunctionBody(d, mg, env.numFuncImports + funcDefIndex)) {
      return false;
    }
  }

  if (!d.finishSection(*env.codeSection, "code")) {
    return false;
  }

  return mg.finishFuncDefs();
}

SharedModule wasm::CompileBuffer(const CompileArgs& args,
                                 const ShareableBytes& bytecode,
                                 UniqueChars* error,
                                 UniqueCharsVector* warnings,
                                 JS::OptimizedEncodingListener* listener) {
  Decoder d(bytecode.bytes, 0, error, warnings);

  ModuleEnvironment moduleEnv(args.features);
  if (!moduleEnv.init() || !DecodeModuleEnvironment(d, &moduleEnv)) {
    return nullptr;
  }
  CompilerEnvironment compilerEnv(args);
  compilerEnv.computeParameters(d);

  ModuleGenerator mg(args, &moduleEnv, &compilerEnv, nullptr, error, warnings);
  if (!mg.init(nullptr)) {
    return nullptr;
  }

  if (!DecodeCodeSection(moduleEnv, d, mg)) {
    return nullptr;
  }

  if (!DecodeModuleTail(d, &moduleEnv)) {
    return nullptr;
  }

  return mg.finishModule(bytecode, listener);
}

bool wasm::CompileTier2(const CompileArgs& args, const Bytes& bytecode,
                        const Module& module, UniqueChars* error,
                        UniqueCharsVector* warnings, Atomic<bool>* cancelled) {
  Decoder d(bytecode, 0, error);

  ModuleEnvironment moduleEnv(args.features);
  if (!moduleEnv.init() || !DecodeModuleEnvironment(d, &moduleEnv)) {
    return false;
  }
  CompilerEnvironment compilerEnv(CompileMode::Tier2, Tier::Optimized,
                                  DebugEnabled::False);
  compilerEnv.computeParameters(d);

  ModuleGenerator mg(args, &moduleEnv, &compilerEnv, cancelled, error,
                     warnings);
  if (!mg.init(nullptr)) {
    return false;
  }

  if (!DecodeCodeSection(moduleEnv, d, mg)) {
    return false;
  }

  if (!DecodeModuleTail(d, &moduleEnv)) {
    return false;
  }

  return mg.finishTier2(module);
}

class StreamingDecoder {
  Decoder d_;
  const ExclusiveBytesPtr& codeBytesEnd_;
  const Atomic<bool>& cancelled_;

 public:
  StreamingDecoder(const ModuleEnvironment& env, const Bytes& begin,
                   const ExclusiveBytesPtr& codeBytesEnd,
                   const Atomic<bool>& cancelled, UniqueChars* error,
                   UniqueCharsVector* warnings)
      : d_(begin, env.codeSection->start, error, warnings),
        codeBytesEnd_(codeBytesEnd),
        cancelled_(cancelled) {}

  bool fail(const char* msg) { return d_.fail(msg); }

  bool done() const { return d_.done(); }

  size_t currentOffset() const { return d_.currentOffset(); }

  bool waitForBytes(size_t numBytes) {
    numBytes = std::min(numBytes, d_.bytesRemain());
    const uint8_t* requiredEnd = d_.currentPosition() + numBytes;
    auto codeBytesEnd = codeBytesEnd_.lock();
    while (codeBytesEnd < requiredEnd) {
      if (cancelled_) {
        return false;
      }
      codeBytesEnd.wait();
    }
    return true;
  }

  bool readVarU32(uint32_t* u32) {
    return waitForBytes(MaxVarU32DecodedBytes) && d_.readVarU32(u32);
  }

  bool readBytes(size_t size, const uint8_t** begin) {
    return waitForBytes(size) && d_.readBytes(size, begin);
  }

  bool finishSection(const SectionRange& range, const char* name) {
    return d_.finishSection(range, name);
  }
};

static SharedBytes CreateBytecode(const Bytes& env, const Bytes& code,
                                  const Bytes& tail, UniqueChars* error) {
  size_t size = env.length() + code.length() + tail.length();
  if (size > MaxModuleBytes) {
    *error = DuplicateString("module too big");
    return nullptr;
  }

  MutableBytes bytecode = js_new<ShareableBytes>();
  if (!bytecode || !bytecode->bytes.resize(size)) {
    return nullptr;
  }

  uint8_t* p = bytecode->bytes.begin();

  memcpy(p, env.begin(), env.length());
  p += env.length();

  memcpy(p, code.begin(), code.length());
  p += code.length();

  memcpy(p, tail.begin(), tail.length());
  p += tail.length();

  MOZ_ASSERT(p == bytecode->end());

  return bytecode;
}

SharedModule wasm::CompileStreaming(
    const CompileArgs& args, const Bytes& envBytes, const Bytes& codeBytes,
    const ExclusiveBytesPtr& codeBytesEnd,
    const ExclusiveStreamEndData& exclusiveStreamEnd,
    const Atomic<bool>& cancelled, UniqueChars* error,
    UniqueCharsVector* warnings) {
  CompilerEnvironment compilerEnv(args);
  ModuleEnvironment moduleEnv(args.features);
  if (!moduleEnv.init()) {
    return nullptr;
  }

  {
    Decoder d(envBytes, 0, error, warnings);

    if (!DecodeModuleEnvironment(d, &moduleEnv)) {
      return nullptr;
    }
    compilerEnv.computeParameters(d);

    if (!moduleEnv.codeSection) {
      d.fail("unknown section before code section");
      return nullptr;
    }

    MOZ_RELEASE_ASSERT(moduleEnv.codeSection->size == codeBytes.length());
    MOZ_RELEASE_ASSERT(d.done());
  }

  ModuleGenerator mg(args, &moduleEnv, &compilerEnv, &cancelled, error,
                     warnings);
  if (!mg.init(nullptr)) {
    return nullptr;
  }

  {
    StreamingDecoder d(moduleEnv, codeBytes, codeBytesEnd, cancelled, error,
                       warnings);

    if (!DecodeCodeSection(moduleEnv, d, mg)) {
      return nullptr;
    }

    MOZ_RELEASE_ASSERT(d.done());
  }

  {
    auto streamEnd = exclusiveStreamEnd.lock();
    while (!streamEnd->reached) {
      if (cancelled) {
        return nullptr;
      }
      streamEnd.wait();
    }
  }

  const StreamEndData& streamEnd = exclusiveStreamEnd.lock();
  const Bytes& tailBytes = *streamEnd.tailBytes;

  {
    Decoder d(tailBytes, moduleEnv.codeSection->end(), error, warnings);

    if (!DecodeModuleTail(d, &moduleEnv)) {
      return nullptr;
    }

    MOZ_RELEASE_ASSERT(d.done());
  }

  SharedBytes bytecode = CreateBytecode(envBytes, codeBytes, tailBytes, error);
  if (!bytecode) {
    return nullptr;
  }

  return mg.finishModule(*bytecode, streamEnd.tier2Listener);
}

class DumpIonModuleGenerator {
 private:
  ModuleEnvironment& moduleEnv_;
  uint32_t targetFuncIndex_;
  IonDumpContents contents_;
  GenericPrinter& out_;
  UniqueChars* error_;

 public:
  DumpIonModuleGenerator(ModuleEnvironment& moduleEnv, uint32_t targetFuncIndex,
                         IonDumpContents contents, GenericPrinter& out,
                         UniqueChars* error)
      : moduleEnv_(moduleEnv),
        targetFuncIndex_(targetFuncIndex),
        contents_(contents),
        out_(out),
        error_(error) {}

  bool finishFuncDefs() { return true; }
  bool compileFuncDef(uint32_t funcIndex, uint32_t lineOrBytecode,
                      const uint8_t* begin, const uint8_t* end) {
    if (funcIndex != targetFuncIndex_) {
      return true;
    }

    FuncCompileInput input(funcIndex, lineOrBytecode, begin, end,
                           Uint32Vector());
    return IonDumpFunction(moduleEnv_, input, contents_, out_, error_);
  }
};

bool wasm::DumpIonFunctionInModule(const ShareableBytes& bytecode,
                                   uint32_t targetFuncIndex,
                                   IonDumpContents contents,
                                   GenericPrinter& out, UniqueChars* error) {
  UniqueCharsVector warnings;
  Decoder d(bytecode.bytes, 0, error, &warnings);
  ModuleEnvironment moduleEnv(FeatureArgs::allEnabled());
  DumpIonModuleGenerator mg(moduleEnv, targetFuncIndex, contents, out, error);
  return moduleEnv.init() && DecodeModuleEnvironment(d, &moduleEnv) &&
         DecodeCodeSection(moduleEnv, d, mg);
}
