/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_FrameIter_inl_h
#define vm_FrameIter_inl_h

#include "vm/FrameIter.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_CRASH

#include "jit/JSJitFrameIter.h"  // js::jit::{InlineFrameIterator,MaybeReadFallback,ReadFrame_Actuals}

#include "vm/Stack-inl.h"  // js::InterpreterFrame::unaliasedForEachActual

namespace js {

template <class Op>
inline void FrameIter::unaliasedForEachActual(JSContext* cx, Op op) {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
      interpFrame()->unaliasedForEachActual(op);
      return;
    case JIT:
      MOZ_ASSERT(isJSJit());
      if (jsJitFrame().isIonJS()) {
        jit::MaybeReadFallback recover(cx, activation()->asJit(),
                                       &jsJitFrame());
        ionInlineFrames_.unaliasedForEachActual(cx, op, recover);
      } else if (jsJitFrame().isBailoutJS()) {
        // :TODO: (Bug 1070962) If we are introspecting the frame which is
        // being bailed, then we might be in the middle of recovering
        // instructions. Stacking computeInstructionResults implies that we
        // might be recovering result twice. In the mean time, to avoid
        // that, we just return Undefined values for instruction results
        // which are not yet recovered.
        jit::MaybeReadFallback fallback;
        ionInlineFrames_.unaliasedForEachActual(cx, op, fallback);
      } else {
        MOZ_ASSERT(jsJitFrame().isBaselineJS());
        jsJitFrame().unaliasedForEachActual(op);
      }
      return;
  }
  MOZ_CRASH("Unexpected state");
}

}  // namespace js

#endif  // vm_FrameIter_inl_h
