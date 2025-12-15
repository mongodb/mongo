/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonOptimizationLevels.h"

#include "jit/Ion.h"
#include "jit/JitHints.h"
#include "jit/JitRuntime.h"
#include "js/Prefs.h"
#include "vm/JSScript.h"

#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

/*static*/
uint32_t OptimizationInfo::baseWarmUpThresholdForScript(JSContext* cx,
                                                        JSScript* script) {
  // If an Ion counter hint is present, override the threshold.
  if (cx->runtime()->jitRuntime()->hasJitHintsMap()) {
    JitHintsMap* jitHints = cx->runtime()->jitRuntime()->getJitHintsMap();
    uint32_t hintThreshold;
    if (jitHints->getIonThresholdHint(script, hintThreshold)) {
      return hintThreshold;
    }
  }
  return JitOptions.normalIonWarmUpThreshold;
}

/*static*/
uint32_t OptimizationInfo::warmUpThresholdForPC(JSScript* script,
                                                jsbytecode* pc,
                                                uint32_t baseThreshold) {
  MOZ_ASSERT(pc == nullptr || pc == script->code() ||
             JSOp(*pc) == JSOp::LoopHead);

  // The script must not start with a LoopHead op or the code below would be
  // wrong. See bug 1602681.
  MOZ_ASSERT_IF(pc && JSOp(*pc) == JSOp::LoopHead, pc > script->code());

  uint32_t warmUpThreshold = baseThreshold;

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
  return warmUpThreshold + loopDepth * (baseThreshold / 10);
}

OptimizationLevel OptimizationLevelInfo::levelForScript(JSContext* cx,
                                                        JSScript* script,
                                                        jsbytecode* pc) const {
  uint32_t baseThreshold =
      OptimizationInfo::baseWarmUpThresholdForScript(cx, script);
  if (script->getWarmUpCount() <
      OptimizationInfo::warmUpThresholdForPC(script, pc, baseThreshold)) {
    return OptimizationLevel::DontCompile;
  }

  return OptimizationLevel::Normal;
}

IonRegisterAllocator OptimizationInfo::registerAllocator() const {
  switch (JS::Prefs::ion_regalloc()) {
    case 0:
    default:
      // Use the default register allocator.
      return registerAllocator_;
    case 1:
      return RegisterAllocator_Backtracking;
    case 2:
      return RegisterAllocator_Simple;
  }
  MOZ_CRASH("Unreachable");
}

}  // namespace jit
}  // namespace js
