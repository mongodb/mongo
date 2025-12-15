/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_WasmHeuristics_h
#define wasm_WasmHeuristics_h

#include <math.h>

#include "js/Prefs.h"
#include "threading/ExclusiveData.h"
#include "vm/MutexIDs.h"
#include "wasm/WasmConstants.h"

namespace js {
namespace wasm {

// Classes LazyTieringHeuristics and InliningHeuristics allow answering of
// simple questions relating to lazy tiering and inlining, eg, "is this
// function small enough to inline?"  They do not answer questions that involve
// carrying state (eg, remaining inlining budget) across multiple queries.
//
// Note also, they may be queried in parallel without locking, by multiple
// instantiating / compilation threads, and so must be immutable once created.

// For both LazyTieringHeuristics and InliningHeuristics, the default `level_`
// is set to 5 in modules/libpref/init/StaticPrefList.yaml.  The scaling
// factors and tables defined in this file have been set so as to give
// near-optimal performance on Barista-3 and another benchmark; they are
// generally within 2% of the best value that can be found by changing the
// `level_` numbers.  Further performance gains may depend on improving the
// accuracy of estimateIonCompilationCost().
//
// Performance was measured on a mid/high-end Intel CPU (Core i5-1135G7 --
// Tiger Lake) and a low end Intel (Celeron N3050 -- Goldmont).

class LazyTieringHeuristics {
  static constexpr uint32_t MIN_LEVEL = 1;
  static constexpr uint32_t MAX_LEVEL = 9;
  static constexpr uint32_t SMALL_MODULE_THRESH = 150000;

  // A scaling table for levels 2 .. 8.  Levels 1 and 9 are special-cased.  In
  // this table, each value differs from its neighbour by a factor of 3, giving
  // a dynamic range in the table of 3 ^ 6 == 729, hence a wide selection of
  // tier-up aggressiveness.
  static constexpr float scale_[7] = {27.0,  9.0,   3.0,
                                      1.0,  // default
                                      0.333, 0.111, 0.037};

 public:
  // 1 = min (almost never, set tiering threshold to max possible, == 2^31-1)
  // 5 = default
  // 9 = max (request tier up at first call, set tiering threshold to zero)
  //
  // Don't use this directly, except for logging etc.
  static uint32_t rawLevel() {
    uint32_t level = JS::Prefs::wasm_lazy_tiering_level();
    return std::clamp(level, MIN_LEVEL, MAX_LEVEL);
  }

  // Estimate the cost of compiling a function of bytecode size `bodyLength`
  // using Ion, in terms of arbitrary work-units.  The baseline code for the
  // function counts down from the returned value as it runs.  When the value
  // goes negative it requests tier-up.  See "[SMDOC] WebAssembly baseline
  // compiler -- Lazy Tier-Up mechanism" in WasmBaselineCompile.cpp.

  static int32_t estimateIonCompilationCost(uint32_t bodyLength,
                                            size_t codeSectionSize) {
    uint32_t level = rawLevel();

    // Increase the aggressiveness of tiering for small modules, since they
    // don't generate much optimised-tier compilation work, so we might as well
    // try to get them into optimized code sooner.  But don't overdo it, since
    // we don't want to lose indirect-target resolution as a result.  See bug
    // 1965195.
    MOZ_ASSERT(codeSectionSize > 0);
    if (codeSectionSize <= SMALL_MODULE_THRESH && level < MAX_LEVEL) {
      level += 1;
    }

    if (MOZ_LIKELY(MIN_LEVEL < level && level < MAX_LEVEL)) {
      // The estimated cost, in X86_64 insns, for Ion compilation:
      // 30k up-front cost + 4k per bytecode byte.
      //
      // This is derived from measurements of an optimized build of Ion
      // compiling about 99000 functions.  Each estimate is pretty bad, but
      // averaged over a number of functions it's often within 20% of correct.
      // However, this is with no inlining; that causes a much wider variance
      // of costs.  This will need to be revisited at some point.
      float thresholdF = 30000.0 + 4000.0 * float(bodyLength);

      // Rescale to step-down work units, so that the default `level` setting
      // (5) gives pretty good results.
      thresholdF *= 0.25;

      // Rescale again to take into account `level`.
      thresholdF *= scale_[level - (MIN_LEVEL + 1)];

      // Clamp and convert.
      constexpr float thresholdHigh = 2.0e9f;  // at most 2 billion;
      int32_t thresholdI = int32_t(std::clamp(thresholdF, 10.f, thresholdHigh));
      MOZ_RELEASE_ASSERT(thresholdI >= 0);
      return thresholdI;
    }
    if (level == MIN_LEVEL) {
      // "almost never tier up"; produce our closest approximation to infinity
      return INT32_MAX;
    }
    if (level == MAX_LEVEL) {
      // request tier up at the first call; return the lowest possible value
      return 0;
    }
    MOZ_CRASH();
  }
};

// [SMDOC] Per-function and per-module inlining limits

// `class InliningHeuristics` makes inlining decisions on a per-call-site
// basis.  Even with that in place, it is still possible to create a small
// input function for which inlining produces a huge (1000 x) expansion.  Hence
// we also need a backstop mechanism to limit growth of functions and of
// modules as a whole.
//
// The following scheme is therefore implemented:
//
// * no function can have an inlining-based expansion of more than a constant
//   factor (here, 99 x).
//
// * for a module as a whole there is also a max expansion factor, and this is
//   much lower, perhaps 1 x.
//
// This means that
//
// * no individual function can cause too much trouble (due to the 99 x limit),
//   yet any function that needs a lot of inlining can still get it. In
//   practice most functions have an inlining expansion, at default settings,
//   of much less than 5 x.
//
// * the module as a whole cannot chew up excessive resources.
//
// Once a limit is exhausted, Ion compilation is still possible, but no
// inlining will be done.
//
// The per-module limit needs to be interpreted in the light of lazy tiering.
// Many modules only tier up a small subset of their functions.  Hence the
// relatively low per-module limit still allows a high level of expansion of
// the functions that do get tiered up.
//
// In effect, the tiering mechanism gives hot functions (early tierer-uppers)
// preferential access to the module-level inlining budget.  Colder functions
// that tier up later may find the budget to be exhausted, in which case they
// get no inlining.  It would be feasible to gradually reduce inlining
// aggressiveness as the budget is used up, rather than have cliff-edge
// behaviour, but it hardly seems worth the hassle.
//
// To implement this, we have
//
// * `int64_t WasmCodeMetadata::ProtectedOptimizationStats::inliningBudget`:
//   this is initially set as the maximum copied-in bytecode length allowable
//   for the module.  Inlining of individual call sites decreases the value and
//   may drive it negative.  Once the value is negative, no more inlining is
//   allowed.
//
// * `int64_t FunctionCompiler::inliningBudget_` does the same at a
//   per-function level.  Its initial value takes into account the current
//   value of the module-level budget; hence if the latter is exhausted, the
//   function-level budget will be zero and so no inlining occurs.
//
// If either limit is exceeded, a message is printed on the
// `MOZ_LOG=wasmCodeMetaStats:3` channel.

// Allowing budgets to be driven negative means we slightly overshoot them.  An
// alternative to be to ensure they can never be driven negative, in which case
// we will slightly undershoot them instead, given that the sum of inlined
// function sizes is unlikely to exactly match the budget.  We use the
// overshoot scheme only because it makes it simple to decide when to log a
// budget-overshoot message and not emit any duplicates.

// There is a (logical, not-TSan-detectable) race condition in that the
// inlining budget for a function is set in part from the module-level budget
// at the time that compilation of the function begins, and the module-level
// budget is updated when compilation of a function ends -- see
// FunctionCompiler::initToplevel and ::finish.  If there are multiple
// compilation threads, it can happen that multiple threads individually
// overrun the module-level budget, and so collectively overshoot the budget
// multiple times.
//
// The worst-case total overshoot is equal to the worst-case per-function
// overshoot multiplied by the max number of functions that can be concurrently
// compiled:
//
//   <max per-function overshoot, which
//      == the largest body length that can be accepted
//             by InliningHeuristics::isSmallEnoughToInline>
//   * MaxPartialTier2CompileTasks
//
// which with current settings is 320 * 1 == 320.
//
// We never expect to hit either limit in normal operation -- they exist only
// to protect against the worst case.  So the imprecision doesn't matter.

// Setting the multiplier here to 1 means that inlining can copy in at maximum
// the same amount of bytecode as is in the module; 2 means twice as much, etc,
// and setting it to 0 would completely disable inlining.
static constexpr int64_t PerModuleMaxInliningRatio = 1;

// Same meaning as above, except at a per-function level.
static constexpr int64_t PerFunctionMaxInliningRatio = 99;

class InliningHeuristics {
  static constexpr uint32_t MIN_LEVEL = 1;
  static constexpr uint32_t MAX_LEVEL = 9;

  static constexpr uint32_t LARGE_FUNCTION_THRESH_1 = 400000;
  static constexpr uint32_t LARGE_FUNCTION_THRESH_2 = 800000;
  static constexpr uint32_t LARGE_FUNCTION_THRESH_3 = 1200000;

 public:
  // 1 = no inlining allowed
  // 2 = min (minimal inlining)
  // 5 = default
  // 9 = max (very aggressive inlining)
  //
  // Don't use these directly, except for logging etc.
  static uint32_t rawLevel() {
    uint32_t level = JS::Prefs::wasm_inlining_level();
    return std::clamp(level, MIN_LEVEL, MAX_LEVEL);
  }
  static bool rawDirectAllowed() { return JS::Prefs::wasm_direct_inlining(); }
  static bool rawCallRefAllowed() {
    return JS::Prefs::wasm_call_ref_inlining();
  }
  // For a call_ref site, returns the percentage of total calls made by that
  // site, that any single target has to make in order to be considered as a
  // candidate for speculative inlining.
  static uint32_t rawCallRefPercent() {
    uint32_t percent = JS::Prefs::wasm_call_ref_inlining_percent();
    // Clamp to range 10 .. 100 (%).
    return std::clamp(percent, 10u, 100u);
  }

  // Calculate the total inlining budget for a module, based on the size of the
  // code section.
  static int64_t moduleInliningBudget(size_t codeSectionSize) {
    int64_t budget = int64_t(codeSectionSize) * PerModuleMaxInliningRatio;

    // Don't be overly stingy for tiny modules.  Function-level inlining
    // limits will still protect us from excessive inlining.
    return std::max<int64_t>(budget, 1000);
  }

  // Given a call of kind `callKind` to a function of bytecode size
  // `bodyLength` at `inliningDepth`, decide whether the it is allowable to
  // inline the call.  Note that `inliningDepth` starts at zero, not one.  In
  // other words, a value of zero means the query relates to a function which
  // (if approved) would be inlined into the top-level function currently being
  // compiled.
  //
  // `rootFunctionBodyLength` is the bytecode size of the function at the root
  // of this inlining stack.  If that is (very) large, we back off somewhat on
  // inlining.  `*largeFunctionBackoff` indicates whether or not that happened.
  enum class CallKind { Direct, CallRef };
  static bool isSmallEnoughToInline(CallKind callKind, uint32_t inliningDepth,
                                    uint32_t bodyLength,
                                    uint32_t rootFunctionBodyLength,
                                    bool* largeFunctionBackoff) {
    *largeFunctionBackoff = false;

    // If this fails, something's seriously wrong; bail out.
    MOZ_RELEASE_ASSERT(inliningDepth <= 10);  // because 10 > (320 / 40)
    MOZ_ASSERT(rootFunctionBodyLength > 0 &&
               rootFunctionBodyLength <= wasm::MaxFunctionBytes);

    // Check whether calls of this kind are currently allowed
    if ((callKind == CallKind::Direct && !rawDirectAllowed()) ||
        (callKind == CallKind::CallRef && !rawCallRefAllowed())) {
      return false;
    }
    // Check the size is allowable.  This depends on how deep we are in the
    // stack and on the setting of level_.  We allow inlining of functions of
    // size up to the `baseSize[]` value at depth zero, but reduce the
    // allowable size by 40 for each further level of inlining, so that only
    // smaller and smaller functions are allowed as we inline deeper.
    //
    // At some point `allowedSize` goes negative and thereby disallows all
    // further inlining.  Note that the `baseSize` entry for
    // `level_ == MIN_LEVEL (== 1)` is set so as to disallow inlining even at
    // depth zero.  Hence `level_ == MIN_LEVEL` disallows all inlining.
    static constexpr int32_t baseSize[9] = {0,   40,  80,  120,
                                            160,  // default
                                            200, 240, 280, 320};
    uint32_t level = rawLevel();

    // If the root function is large, back off somewhat on inlining, so as to
    // limit its further growth.  The limits are set so high that almost all
    // functions will be unaffected by this.  See bug 1967644.
    if (rootFunctionBodyLength > LARGE_FUNCTION_THRESH_1 && level > MIN_LEVEL) {
      level--;
      *largeFunctionBackoff = true;
    }
    if (rootFunctionBodyLength > LARGE_FUNCTION_THRESH_2 && level > MIN_LEVEL) {
      level--;
      *largeFunctionBackoff = true;
    }
    if (rootFunctionBodyLength > LARGE_FUNCTION_THRESH_3 && level > MIN_LEVEL) {
      level--;
      *largeFunctionBackoff = true;
    }

    // Having established `level`, check whether the callee is small enough.
    MOZ_RELEASE_ASSERT(level >= MIN_LEVEL && level <= MAX_LEVEL);
    int32_t allowedSize = baseSize[level - MIN_LEVEL];
    allowedSize -= int32_t(40 * inliningDepth);
    return allowedSize > 0 && bodyLength <= uint32_t(allowedSize);
  }
};

}  // namespace wasm
}  // namespace js

#endif /* wasm_WasmHeuristics_h */
