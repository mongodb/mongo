/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ICState_h
#define jit_ICState_h

#include "jit/JitOptions.h"

namespace js {
namespace jit {

// ICState stores information about a Baseline or Ion IC.
class ICState
{
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
    Mode mode_;

    // Number of optimized stubs currently attached to this IC.
    uint8_t numOptimizedStubs_;

    // Number of times we failed to attach a stub.
    uint8_t numFailures_;

    // This is only used for shared Baseline ICs and stored here to save space.
    bool invalid_ : 1;

    static const size_t MaxOptimizedStubs = 6;

    void transition(Mode mode) {
        MOZ_ASSERT(mode > mode_);
        mode_ = mode;
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
    ICState()
      : invalid_(false)
    {
        reset();
    }

    Mode mode() const { return mode_; }
    size_t numOptimizedStubs() const { return numOptimizedStubs_; }

    MOZ_ALWAYS_INLINE bool canAttachStub() const {
        // Note: we cannot assert that numOptimizedStubs_ <= MaxOptimizedStubs
        // because old-style baseline ICs may attach more stubs than
        // MaxOptimizedStubs allows.
        if (mode_ == Mode::Generic || JitOptions.disableCacheIR)
            return false;
        return true;
    }

    bool invalid() const { return invalid_; }
    void setInvalid() { invalid_ = true; }

    // If this returns true, we transitioned to a new mode and the caller
    // should discard all stubs.
    MOZ_MUST_USE MOZ_ALWAYS_INLINE bool maybeTransition() {
        // Note: we cannot assert that numOptimizedStubs_ <= MaxOptimizedStubs
        // because old-style baseline ICs may attach more stubs than
        // MaxOptimizedStubs allows.
        if (mode_ == Mode::Generic)
            return false;
        if (numOptimizedStubs_ < MaxOptimizedStubs && numFailures_ < maxFailures())
            return false;
        if (numFailures_ == maxFailures() || mode_ == Mode::Megamorphic) {
            transition(Mode::Generic);
            return true;
        }
        MOZ_ASSERT(mode_ == Mode::Specialized);
        transition(Mode::Megamorphic);
        return true;
    }
    void reset() {
        mode_ = Mode::Specialized;
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
        numFailures_ = 0;
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
    void trackUnlinkedAllStubs() {
        numOptimizedStubs_ = 0;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_ICState_h */
