/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitOptions.h"
#include "mozilla/TypeTraits.h"

#include <cstdlib>
#include "jsfun.h"
using namespace js;
using namespace js::jit;

using mozilla::Maybe;

namespace js {
namespace jit {

DefaultJitOptions JitOptions;

static void Warn(const char* env, const char* value)
{
    fprintf(stderr, "Warning: I didn't understand %s=\"%s\"\n", env, value);
}

template<typename T> struct IsBool : mozilla::FalseType {};
template<> struct IsBool<bool> : mozilla::TrueType {};

static Maybe<int>
ParseInt(const char* str)
{
    char* endp;
    int retval = strtol(str, &endp, 0);
    if (*endp == '\0')
        return mozilla::Some(retval);
    return mozilla::Nothing();
}

template<typename T>
T overrideDefault(const char* param, T dflt) {
    char* str = getenv(param);
    if (!str)
        return dflt;
    if (IsBool<T>::value) {
        if (strcmp(str, "true") == 0 || strcmp(str, "yes") == 0)
            return true;
        if (strcmp(str, "false") == 0 || strcmp(str, "no") == 0)
            return false;
        Warn(param, str);
    } else {
        Maybe<int> value = ParseInt(str);
        if (value.isSome())
            return value.ref();
        Warn(param, str);
    }
    return dflt;
}
#define SET_DEFAULT(var, dflt) var = overrideDefault("JIT_OPTION_" #var, dflt)
DefaultJitOptions::DefaultJitOptions()
{
    // Whether to perform expensive graph-consistency DEBUG-only assertions.
    // It can be useful to disable this to reduce DEBUG-compile time of large
    // asm.js programs.
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

    // Toggle whether eager simd unboxing is globally disabled.
    SET_DEFAULT(disableEagerSimdUnbox, false);

    // Toggles whether Edge Case Analysis is gobally disabled.
    SET_DEFAULT(disableEdgeCaseAnalysis, false);

    // Toggle whether global value numbering is globally disabled.
    SET_DEFAULT(disableGvn, false);

    // Toggles whether inlining is globally disabled.
    SET_DEFAULT(disableInlining, false);

    // Toggles whether loop invariant code motion is globally disabled.
    SET_DEFAULT(disableLicm, false);

    // Toggles whether Loop Unrolling is globally disabled.
    SET_DEFAULT(disableLoopUnrolling, true);

    // Toggle whether Profile Guided Optimization is globally disabled.
    SET_DEFAULT(disablePgo, true);

    // Toggles whether instruction reordering is globally disabled.
    SET_DEFAULT(disableInstructionReordering, false);

    // Toggles whether Range Analysis is globally disabled.
    SET_DEFAULT(disableRangeAnalysis, false);

    // Toggle whether eager scalar replacement is globally disabled.
    SET_DEFAULT(disableScalarReplacement, false);

    // Toggles whether shared stubs are used in Ionmonkey.
    SET_DEFAULT(disableSharedStubs, true);

    // Toggles whether sincos optimization is globally disabled.
    // See bug984018: The MacOS is the only one that has the sincos fast.
    #if defined(XP_MACOSX)
        SET_DEFAULT(disableSincos, false);
    #else
        SET_DEFAULT(disableSincos, true);
    #endif

    // Toggles whether sink code motion is globally disabled.
    SET_DEFAULT(disableSink, true);

    // Whether functions are compiled immediately.
    SET_DEFAULT(eagerCompilation, false);

    // Whether IonBuilder should prefer IC generation above specialized MIR.
    SET_DEFAULT(forceInlineCaches, false);

    // Toggles whether large scripts are rejected.
    SET_DEFAULT(limitScriptSize, true);

    // Toggles whether functions may be entered at loop headers.
    SET_DEFAULT(osr, true);

    // Whether to enable extra code to perform dynamic validations.
    SET_DEFAULT(runExtraChecks, false);

    // How many invocations or loop iterations are needed before functions
    // are compiled with the baseline compiler.
    SET_DEFAULT(baselineWarmUpThreshold, 10);

    // Number of exception bailouts (resuming into catch/finally block) before
    // we invalidate and forbid Ion compilation.
    SET_DEFAULT(exceptionBailoutThreshold, 10);

    // Number of bailouts without invalidation before we set
    // JSScript::hadFrequentBailouts and invalidate.
    SET_DEFAULT(frequentBailoutThreshold, 10);

    // How many actual arguments are accepted on the C stack.
    SET_DEFAULT(maxStackArgs, 4096);

    // How many times we will try to enter a script via OSR before
    // invalidating the script.
    SET_DEFAULT(osrPcMismatchesBeforeRecompile, 6000);

    // The bytecode length limit for small function.
    SET_DEFAULT(smallFunctionMaxBytecodeLength_, 120);

    // Force how many invocation or loop iterations are needed before compiling
    // a function with the highest ionmonkey optimization level.
    // (i.e. OptimizationLevel_Normal)
    const char* forcedDefaultIonWarmUpThresholdEnv = "JIT_OPTION_forcedDefaultIonWarmUpThreshold";
    if (const char* env = getenv(forcedDefaultIonWarmUpThresholdEnv)) {
        Maybe<int> value = ParseInt(env);
        if (value.isSome())
            forcedDefaultIonWarmUpThreshold.emplace(value.ref());
        else
            Warn(forcedDefaultIonWarmUpThresholdEnv, env);
    }

    // Force the used register allocator instead of letting the optimization
    // pass decide.
    const char* forcedRegisterAllocatorEnv = "JIT_OPTION_forcedRegisterAllocator";
    if (const char* env = getenv(forcedRegisterAllocatorEnv)) {
        forcedRegisterAllocator = LookupRegisterAllocator(env);
        if (!forcedRegisterAllocator.isSome())
            Warn(forcedRegisterAllocatorEnv, env);
    }

    // Toggles whether unboxed plain objects can be created by the VM.
    SET_DEFAULT(disableUnboxedObjects, false);
}

bool
DefaultJitOptions::isSmallFunction(JSScript* script) const
{
    return script->length() <= smallFunctionMaxBytecodeLength_;
}

void
DefaultJitOptions::enableGvn(bool enable)
{
    disableGvn = !enable;
}

void
DefaultJitOptions::setEagerCompilation()
{
    eagerCompilation = true;
    baselineWarmUpThreshold = 0;
    forcedDefaultIonWarmUpThreshold.reset();
    forcedDefaultIonWarmUpThreshold.emplace(0);
}

void
DefaultJitOptions::setCompilerWarmUpThreshold(uint32_t warmUpThreshold)
{
    forcedDefaultIonWarmUpThreshold.reset();
    forcedDefaultIonWarmUpThreshold.emplace(warmUpThreshold);

    // Undo eager compilation
    if (eagerCompilation && warmUpThreshold != 0) {
        jit::DefaultJitOptions defaultValues;
        eagerCompilation = false;
        baselineWarmUpThreshold = defaultValues.baselineWarmUpThreshold;
    }
}

void
DefaultJitOptions::resetCompilerWarmUpThreshold()
{
    forcedDefaultIonWarmUpThreshold.reset();

    // Undo eager compilation
    if (eagerCompilation) {
        jit::DefaultJitOptions defaultValues;
        eagerCompilation = false;
        baselineWarmUpThreshold = defaultValues.baselineWarmUpThreshold;
    }
}

} // namespace jit
} // namespace js
