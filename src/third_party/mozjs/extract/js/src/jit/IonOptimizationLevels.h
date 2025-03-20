/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonOptimizationLevels_h
#define jit_IonOptimizationLevels_h

#include "mozilla/EnumeratedArray.h"

#include "jstypes.h"

#include "jit/JitOptions.h"
#include "js/TypeDecls.h"

namespace js {
namespace jit {

enum class OptimizationLevel : uint8_t { Normal, Wasm, Count, DontCompile };

#ifdef JS_JITSPEW
inline const char* OptimizationLevelString(OptimizationLevel level) {
  switch (level) {
    case OptimizationLevel::DontCompile:
      return "Optimization_DontCompile";
    case OptimizationLevel::Normal:
      return "Optimization_Normal";
    case OptimizationLevel::Wasm:
      return "Optimization_Wasm";
    case OptimizationLevel::Count:;
  }
  MOZ_CRASH("Invalid OptimizationLevel");
}
#endif

// Class representing the Ion optimization settings for an OptimizationLevel.
class OptimizationInfo {
  OptimizationLevel level_;

  // Toggles whether Effective Address Analysis is performed.
  bool eaa_;

  // Toggles whether Alignment Mask Analysis is performed.
  bool ama_;

  // Toggles whether Edge Case Analysis is used.
  bool edgeCaseAnalysis_;

  // Toggles whether redundant checks get removed.
  bool eliminateRedundantChecks_;

  // Toggles whether redundant shape guards get removed.
  bool eliminateRedundantShapeGuards_;

  // Toggles whether redundant GC barriers get removed.
  bool eliminateRedundantGCBarriers_;

  // Toggles whether interpreted scripts get inlined.
  bool inlineInterpreted_;

  // Toggles whether native scripts get inlined.
  bool inlineNative_;

  // Toggles whether global value numbering is used.
  bool gvn_;

  // Toggles whether loop invariant code motion is performed.
  bool licm_;

  // Toggles whether Range Analysis is used.
  bool rangeAnalysis_;

  // Toggles whether instruction reordering is performed.
  bool reordering_;

  // Toggles whether Truncation based on Range Analysis is used.
  bool autoTruncate_;

  // Toggles whether sink is used.
  bool sink_;

  // Toggles whether scalar replacement is used.
  bool scalarReplacement_;

  // Describes which register allocator to use.
  IonRegisterAllocator registerAllocator_;

  uint32_t baseCompilerWarmUpThreshold() const {
    MOZ_ASSERT(level_ == OptimizationLevel::Normal);
    return JitOptions.normalIonWarmUpThreshold;
  }

 public:
  constexpr OptimizationInfo()
      : level_(OptimizationLevel::Normal),
        eaa_(false),
        ama_(false),
        edgeCaseAnalysis_(false),
        eliminateRedundantChecks_(false),
        eliminateRedundantShapeGuards_(false),
        eliminateRedundantGCBarriers_(false),
        inlineInterpreted_(false),
        inlineNative_(false),
        gvn_(false),
        licm_(false),
        rangeAnalysis_(false),
        reordering_(false),
        autoTruncate_(false),
        sink_(false),
        scalarReplacement_(false),
        registerAllocator_(RegisterAllocator_Backtracking) {}

  void initNormalOptimizationInfo();
  void initWasmOptimizationInfo();

  OptimizationLevel level() const { return level_; }

  bool inlineInterpreted() const {
    return inlineInterpreted_ && !JitOptions.disableInlining;
  }

  bool inlineNative() const {
    return inlineNative_ && !JitOptions.disableInlining;
  }

  uint32_t compilerWarmUpThreshold(JSContext* cx, JSScript* script,
                                   jsbytecode* pc = nullptr) const;

  uint32_t recompileWarmUpThreshold(JSContext* cx, JSScript* script,
                                    jsbytecode* pc) const;

  bool gvnEnabled() const { return gvn_ && !JitOptions.disableGvn; }

  bool licmEnabled() const { return licm_ && !JitOptions.disableLicm; }

  bool rangeAnalysisEnabled() const {
    return rangeAnalysis_ && !JitOptions.disableRangeAnalysis;
  }

  bool instructionReorderingEnabled() const {
    return reordering_ && !JitOptions.disableInstructionReordering;
  }

  bool autoTruncateEnabled() const {
    return autoTruncate_ && rangeAnalysisEnabled();
  }

  bool sinkEnabled() const { return sink_ && !JitOptions.disableSink; }

  bool eaaEnabled() const { return eaa_ && !JitOptions.disableEaa; }

  bool amaEnabled() const { return ama_ && !JitOptions.disableAma; }

  bool edgeCaseAnalysisEnabled() const {
    return edgeCaseAnalysis_ && !JitOptions.disableEdgeCaseAnalysis;
  }

  bool eliminateRedundantChecksEnabled() const {
    return eliminateRedundantChecks_;
  }

  bool eliminateRedundantShapeGuardsEnabled() const {
    return eliminateRedundantShapeGuards_ &&
           !JitOptions.disableRedundantShapeGuards;
  }

  bool eliminateRedundantGCBarriersEnabled() const {
    return eliminateRedundantGCBarriers_ &&
           !JitOptions.disableRedundantGCBarriers;
  }

  IonRegisterAllocator registerAllocator() const {
    return JitOptions.forcedRegisterAllocator.valueOr(registerAllocator_);
  }

  bool scalarReplacementEnabled() const {
    return scalarReplacement_ && !JitOptions.disableScalarReplacement;
  }
};

class OptimizationLevelInfo {
 private:
  mozilla::EnumeratedArray<OptimizationLevel, OptimizationInfo,
                           size_t(OptimizationLevel::Count)>
      infos_;

 public:
  OptimizationLevelInfo();

  const OptimizationInfo* get(OptimizationLevel level) const {
    return &infos_[level];
  }

  OptimizationLevel levelForScript(JSContext* cx, JSScript* script,
                                   jsbytecode* pc = nullptr) const;
};

extern const OptimizationLevelInfo IonOptimizations;

}  // namespace jit
}  // namespace js

#endif /* jit_IonOptimizationLevels_h */
