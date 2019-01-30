/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonOptimizationLevels.h"

#include "jit/Ion.h"
#include "vm/JSScript.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

OptimizationLevelInfo IonOptimizations;

const uint32_t OptimizationInfo::CompilerWarmupThreshold = 1000;
const uint32_t OptimizationInfo::CompilerSmallFunctionWarmupThreshold = CompilerWarmupThreshold;

void
OptimizationInfo::initNormalOptimizationInfo()
{
    level_ = OptimizationLevel::Normal;

    autoTruncate_ = true;
    eaa_ = true;
    eagerSimdUnbox_ = true;
    edgeCaseAnalysis_ = true;
    eliminateRedundantChecks_ = true;
    inlineInterpreted_ = true;
    inlineNative_ = true;
    licm_ = true;
    loopUnrolling_ = true;
    gvn_ = true;
    rangeAnalysis_ = true;
    reordering_ = true;
    sincos_ = true;
    sink_ = true;

    registerAllocator_ = RegisterAllocator_Backtracking;

    inlineMaxBytecodePerCallSiteActiveCooperatingThread_ = 550;
    inlineMaxBytecodePerCallSiteHelperThread_ = 1100;
    inlineMaxCalleeInlinedBytecodeLength_ = 3550;
    inlineMaxTotalBytecodeLength_ = 85000;
    inliningMaxCallerBytecodeLength_ = 1600;
    maxInlineDepth_ = 3;
    scalarReplacement_ = true;
    smallFunctionMaxInlineDepth_ = 10;
    compilerWarmUpThreshold_ = CompilerWarmupThreshold;
    compilerSmallFunctionWarmUpThreshold_ = CompilerSmallFunctionWarmupThreshold;
    inliningWarmUpThresholdFactor_ = 0.125;
    inliningRecompileThresholdFactor_ = 4;
}

void
OptimizationInfo::initWasmOptimizationInfo()
{
    // The Wasm optimization level
    // Disables some passes that don't work well with wasm.

    // Take normal option values for not specified values.
    initNormalOptimizationInfo();

    level_ = OptimizationLevel::Wasm;

    ama_ = true;
    autoTruncate_ = false;
    eagerSimdUnbox_ = false;           // wasm has no boxing / unboxing.
    edgeCaseAnalysis_ = false;
    eliminateRedundantChecks_ = false;
    scalarReplacement_ = false;        // wasm has no objects.
    sincos_ = false;
    sink_ = false;
}

uint32_t
OptimizationInfo::compilerWarmUpThreshold(JSScript* script, jsbytecode* pc) const
{
    MOZ_ASSERT(pc == nullptr || pc == script->code() || JSOp(*pc) == JSOP_LOOPENTRY);

    if (pc == script->code())
        pc = nullptr;

    uint32_t warmUpThreshold = JitOptions.forcedDefaultIonWarmUpThreshold
        .valueOr(compilerWarmUpThreshold_);

    if (JitOptions.isSmallFunction(script)) {
        warmUpThreshold = JitOptions.forcedDefaultIonSmallFunctionWarmUpThreshold
            .valueOr(compilerSmallFunctionWarmUpThreshold_);
    }

    // If the script is too large to compile on the active thread, we can still
    // compile it off thread. In these cases, increase the warm-up counter
    // threshold to improve the compilation's type information and hopefully
    // avoid later recompilation.

    if (script->length() > MAX_ACTIVE_THREAD_SCRIPT_SIZE)
        warmUpThreshold *= (script->length() / (double) MAX_ACTIVE_THREAD_SCRIPT_SIZE);

    uint32_t numLocalsAndArgs = NumLocalsAndArgs(script);
    if (numLocalsAndArgs > MAX_ACTIVE_THREAD_LOCALS_AND_ARGS)
        warmUpThreshold *= (numLocalsAndArgs / (double) MAX_ACTIVE_THREAD_LOCALS_AND_ARGS);

    if (!pc || JitOptions.eagerCompilation)
        return warmUpThreshold;

    // It's more efficient to enter outer loops, rather than inner loops, via OSR.
    // To accomplish this, we use a slightly higher threshold for inner loops.
    // Note that the loop depth is always > 0 so we will prefer non-OSR over OSR.
    uint32_t loopDepth = LoopEntryDepthHint(pc);
    MOZ_ASSERT(loopDepth > 0);
    return warmUpThreshold + loopDepth * 100;
}

OptimizationLevelInfo::OptimizationLevelInfo()
{
    infos_[OptimizationLevel::Normal].initNormalOptimizationInfo();
    infos_[OptimizationLevel::Wasm].initWasmOptimizationInfo();

#ifdef DEBUG
    OptimizationLevel level = firstLevel();
    while (!isLastLevel(level)) {
        OptimizationLevel next = nextLevel(level);
        MOZ_ASSERT_IF(level != OptimizationLevel::DontCompile, level < next);
        level = next;
    }
#endif
}

OptimizationLevel
OptimizationLevelInfo::nextLevel(OptimizationLevel level) const
{
    MOZ_ASSERT(!isLastLevel(level));
    switch (level) {
      case OptimizationLevel::DontCompile:
        return OptimizationLevel::Normal;
      case OptimizationLevel::Normal:
      case OptimizationLevel::Wasm:
      case OptimizationLevel::Count:;
    }
    MOZ_CRASH("Unknown optimization level.");
}

OptimizationLevel
OptimizationLevelInfo::firstLevel() const
{
    return nextLevel(OptimizationLevel::DontCompile);
}

bool
OptimizationLevelInfo::isLastLevel(OptimizationLevel level) const
{
    return level == OptimizationLevel::Normal;
}

OptimizationLevel
OptimizationLevelInfo::levelForScript(JSScript* script, jsbytecode* pc) const
{
    OptimizationLevel prev = OptimizationLevel::DontCompile;

    while (!isLastLevel(prev)) {
        OptimizationLevel level = nextLevel(prev);
        const OptimizationInfo* info = get(level);
        if (script->getWarmUpCount() < info->compilerWarmUpThreshold(script, pc))
            return prev;

        prev = level;
    }

    return prev;
}

} // namespace jit
} // namespace js
