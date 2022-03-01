/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ICState_h
#define jit_ICState_h

#include "jit/JitOptions.h"

namespace js {
namespace jit {

// Used to track trial inlining status for a Baseline IC.
// See also setTrialInliningState below.
enum class TrialInliningState : uint8_t {
  Initial = 0,
  Candidate,
  Inlined,
  Failure,
};

// ICState stores information about a Baseline or Ion IC.
class ICState {
 public:
  // When we attach the maximum number of stubs, we discard all stubs and
  // transition the IC to Megamorphic to attach stubs that are more generic
  // (handle more cases). If we again attach the maximum number of stubs, we
  // transition to Generic and (depending on the IC) will either attach a
  // single stub that handles everything or stop attaching new stubs.
  //
  // We also transition to Generic when we repeatedly fail to attach a stub,
  // to avoid wasting time trying.
  enum class Mode : uint8_t { Specialized = 0, Megamorphic, Generic };

 private:
  uint8_t mode_ : 2;

  // The TrialInliningState for a Baseline IC.
  uint8_t trialInliningState_ : 2;

  // Whether WarpOracle created a snapshot based on stubs attached to this
  // Baseline IC.
  bool usedByTranspiler_ : 1;

  // Number of optimized stubs currently attached to this IC.
  uint8_t numOptimizedStubs_;

  // Number of times we failed to attach a stub.
  uint8_t numFailures_;

  static const size_t MaxOptimizedStubs = 6;

  void setMode(Mode mode) {
    mode_ = uint32_t(mode);
    MOZ_ASSERT(Mode(mode_) == mode, "mode must fit in bitfield");
  }

  void transition(Mode mode) {
    MOZ_ASSERT(mode > this->mode());
    setMode(mode);
    numFailures_ = 0;
  }

  MOZ_ALWAYS_INLINE size_t maxFailures() const {
    // Allow more failures if we attached stubs.
    static_assert(MaxOptimizedStubs == 6,
                  "numFailures_/maxFailures should fit in uint8_t");
    size_t res = 5 + size_t(40) * numOptimizedStubs_;
    MOZ_ASSERT(res <= UINT8_MAX, "numFailures_ should not overflow");
    return res;
  }

 public:
  ICState() { reset(); }

  Mode mode() const { return Mode(mode_); }
  size_t numOptimizedStubs() const { return numOptimizedStubs_; }
  bool hasFailures() const { return (numFailures_ != 0); }
  bool newStubIsFirstStub() const {
    return (mode() == Mode::Specialized && numOptimizedStubs() == 0);
  }

  MOZ_ALWAYS_INLINE bool canAttachStub() const {
    // Note: we cannot assert that numOptimizedStubs_ <= MaxOptimizedStubs
    // because old-style baseline ICs may attach more stubs than
    // MaxOptimizedStubs allows.
    if (mode() == Mode::Generic || JitOptions.disableCacheIR) {
      return false;
    }
    return true;
  }

  // If this returns true, we transitioned to a new mode and the caller
  // should discard all stubs.
  [[nodiscard]] MOZ_ALWAYS_INLINE bool maybeTransition() {
    // Note: we cannot assert that numOptimizedStubs_ <= MaxOptimizedStubs
    // because old-style baseline ICs may attach more stubs than
    // MaxOptimizedStubs allows.
    if (mode() == Mode::Generic) {
      return false;
    }
    if (numOptimizedStubs_ < MaxOptimizedStubs &&
        numFailures_ < maxFailures()) {
      return false;
    }
    if (numFailures_ == maxFailures() || mode() == Mode::Megamorphic) {
      transition(Mode::Generic);
      return true;
    }
    MOZ_ASSERT(mode() == Mode::Specialized);
    transition(Mode::Megamorphic);
    return true;
  }

  void reset() {
    setMode(Mode::Specialized);
#ifdef DEBUG
    if (JitOptions.forceMegamorphicICs) {
      setMode(Mode::Megamorphic);
    }
#endif
    trialInliningState_ = uint32_t(TrialInliningState::Initial);
    usedByTranspiler_ = false;
    numOptimizedStubs_ = 0;
    numFailures_ = 0;
  }
  void trackAttached() {
    // We'd like to assert numOptimizedStubs_ < MaxOptimizedStubs, but
    // since this code is also used for non-CacheIR Baseline stubs, assert
    // < 16 for now. Note that we do have the stronger assert in other
    // methods, because they are only used by CacheIR ICs.
    MOZ_ASSERT(numOptimizedStubs_ < 16);
    numOptimizedStubs_++;
    // As a heuristic, reduce the failure count after each successful attach
    // to delay hitting Generic mode. Reset to 1 instead of 0 so that
    // BaselineInspector can distinguish no-failures from rare-failures.
    numFailures_ = std::min(numFailures_, static_cast<uint8_t>(1));
  }
  void trackNotAttached() {
    // Note: we can't assert numFailures_ < maxFailures() because
    // maxFailures() depends on numOptimizedStubs_ and it's possible a
    // GC discarded stubs before we got here.
    numFailures_++;
    MOZ_ASSERT(numFailures_ > 0, "numFailures_ should not overflow");
  }
  void trackUnlinkedStub() {
    MOZ_ASSERT(numOptimizedStubs_ > 0);
    numOptimizedStubs_--;
  }
  void trackUnlinkedAllStubs() { numOptimizedStubs_ = 0; }

  void clearUsedByTranspiler() { usedByTranspiler_ = false; }
  void setUsedByTranspiler() { usedByTranspiler_ = true; }
  bool usedByTranspiler() const { return usedByTranspiler_; }

  TrialInliningState trialInliningState() const {
    return TrialInliningState(trialInliningState_);
  }
  void setTrialInliningState(TrialInliningState state) {
#ifdef DEBUG
    // Moving to the Failure state is always valid. The other states should
    // happen in this order:
    //
    //   Initial -> Candidate -> Inlined
    //
    // This ensures we perform trial inlining at most once per IC site.
    if (state != TrialInliningState::Failure) {
      switch (trialInliningState()) {
        case TrialInliningState::Initial:
          MOZ_ASSERT(state == TrialInliningState::Candidate);
          break;
        case TrialInliningState::Candidate:
          MOZ_ASSERT(state == TrialInliningState::Candidate ||
                     state == TrialInliningState::Inlined);
          break;
        case TrialInliningState::Inlined:
        case TrialInliningState::Failure:
          MOZ_CRASH("Inlined and Failure can only change to Failure");
          break;
      }
    }
#endif

    trialInliningState_ = uint32_t(state);
    MOZ_ASSERT(trialInliningState() == state,
               "TrialInliningState must fit in bitfield");
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_ICState_h */
