/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonOptimizationLevels.h"

#include "jit/Ion.h"
#include "jit/JitHints.h"
#include "jit/JitRuntime.h"
#include "vm/JSScript.h"

#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

const OptimizationLevelInfo IonOptimizations;

void OptimizationInfo::initNormalOptimizationInfo() {
  level_ = OptimizationLevel::Normal;

  autoTruncate_ = true;
  eaa_ = true;
  edgeCaseAnalysis_ = true;
  eliminateRedundantChecks_ = true;
  eliminateRedundantShapeGuards_ = true;
  eliminateRedundantGCBarriers_ = true;
  inlineInterpreted_ = true;
  inlineNative_ = true;
  licm_ = true;
  gvn_ = true;
  rangeAnalysis_ = true;
  reordering_ = true;
  scalarReplacement_ = true;
  sink_ = true;

  registerAllocator_ = RegisterAllocator_Backtracking;
}

void OptimizationInfo::initWasmOptimizationInfo() {
  // The Wasm optimization level
  // Disables some passes that don't work well with wasm.

  // Take normal option values for not specified values.
  initNormalOptimizationInfo();

  level_ = OptimizationLevel::Wasm;

  ama_ = true;
  autoTruncate_ = false;
  edgeCaseAnalysis_ = false;
  eliminateRedundantChecks_ = false;
  eliminateRedundantShapeGuards_ = false;
  eliminateRedundantGCBarriers_ = false;
  scalarReplacement_ = false;  // wasm has no objects.
  sink_ = false;
}

uint32_t OptimizationInfo::compilerWarmUpThreshold(JSContext* cx,
                                                   JSScript* script,
                                                   jsbytecode* pc) const {
  MOZ_ASSERT(pc == nullptr || pc == script->code() ||
             JSOp(*pc) == JSOp::LoopHead);

  // The script must not start with a LoopHead op or the code below would be
  // wrong. See bug 1602681.
  MOZ_ASSERT_IF(pc && JSOp(*pc) == JSOp::LoopHead, pc > script->code());

  uint32_t warmUpThreshold = baseCompilerWarmUpThreshold();

  // If an Ion counter hint is present, override the threshold.
  if (cx->runtime()->jitRuntime()->hasJitHintsMap()) {
    JitHintsMap* jitHints = cx->runtime()->jitRuntime()->getJitHintsMap();
    uint32_t hintThreshold;
    if (jitHints->getIonThresholdHint(script, hintThreshold)) {
      warmUpThreshold = hintThreshold;
    }
  }

  if (pc == script->code()) {
    pc = nullptr;
  }

  // If the script is too large to compile on the main thread, we can still
  // compile it off thread. In these cases, increase the warm-up counter
  // threshold to improve the compilation's type information and hopefully
  // avoid later recompilation.

  if (script->length() > JitOptions.ionMaxScriptSizeMainThread) {
    warmUpThreshold *=
        (script->length() / double(JitOptions.ionMaxScriptSizeMainThread));
  }

  uint32_t numLocalsAndArgs = NumLocalsAndArgs(script);
  if (numLocalsAndArgs > JitOptions.ionMaxLocalsAndArgsMainThread) {
    warmUpThreshold *=
        (numLocalsAndArgs / double(JitOptions.ionMaxLocalsAndArgsMainThread));
  }

  if (!pc || JitOptions.eagerIonCompilation()) {
    return warmUpThreshold;
  }

  // It's more efficient to enter outer loops, rather than inner loops, via OSR.
  // To accomplish this, we use a slightly higher threshold for inner loops.
  // Note that the loop depth is always > 0 so we will prefer non-OSR over OSR.
  uint32_t loopDepth = LoopHeadDepthHint(pc);
  MOZ_ASSERT(loopDepth > 0);
  return warmUpThreshold + loopDepth * (baseCompilerWarmUpThreshold() / 10);
}

uint32_t OptimizationInfo::recompileWarmUpThreshold(JSContext* cx,
                                                    JSScript* script,
                                                    jsbytecode* pc) const {
  MOZ_ASSERT(pc == script->code() || JSOp(*pc) == JSOp::LoopHead);

  uint32_t threshold = compilerWarmUpThreshold(cx, script, pc);
  if (JSOp(*pc) != JSOp::LoopHead || JitOptions.eagerIonCompilation()) {
    return threshold;
  }

  // If we're stuck in a long-running loop at a low optimization level, we have
  // to invalidate to be able to tier up. This is worse than recompiling at
  // function entry (because in that case we can use the lazy link mechanism and
  // avoid invalidation completely). Use a very high recompilation threshold for
  // loop edges so that this only affects very long-running loops.

  uint32_t loopDepth = LoopHeadDepthHint(pc);
  MOZ_ASSERT(loopDepth > 0);
  return threshold + loopDepth * (baseCompilerWarmUpThreshold() / 10);
}

OptimizationLevelInfo::OptimizationLevelInfo() {
  infos_[OptimizationLevel::Normal].initNormalOptimizationInfo();
  infos_[OptimizationLevel::Wasm].initWasmOptimizationInfo();
}

OptimizationLevel OptimizationLevelInfo::levelForScript(JSContext* cx,
                                                        JSScript* script,
                                                        jsbytecode* pc) const {
  const OptimizationInfo* info = get(OptimizationLevel::Normal);
  if (script->getWarmUpCount() <
      info->compilerWarmUpThreshold(cx, script, pc)) {
    return OptimizationLevel::DontCompile;
  }

  return OptimizationLevel::Normal;
}

}  // namespace jit
}  // namespace js
