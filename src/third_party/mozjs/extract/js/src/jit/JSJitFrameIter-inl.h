/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JSJitFrameIter_inl_h
#define jit_JSJitFrameIter_inl_h

#include "jit/JSJitFrameIter.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/ScriptFromCalleeToken.h"

namespace js {
namespace jit {

inline uint8_t* JSJitFrameIter::returnAddress() const {
  CommonFrameLayout* current = (CommonFrameLayout*)current_;
  return current->returnAddress();
}

inline FrameType JSJitFrameIter::prevType() const {
  CommonFrameLayout* current = (CommonFrameLayout*)current_;
  return current->prevType();
}

inline ExitFrameLayout* JSJitFrameIter::exitFrame() const {
  MOZ_ASSERT(isExitFrame());
  return (ExitFrameLayout*)fp();
}

inline JitFrameLayout* JSJitProfilingFrameIterator::framePtr() const {
  MOZ_ASSERT(!done());
  return (JitFrameLayout*)fp_;
}

inline JSScript* JSJitProfilingFrameIterator::frameScript() const {
  return ScriptFromCalleeToken(framePtr()->calleeToken());
}

inline BaselineFrame* JSJitFrameIter::baselineFrame() const {
  MOZ_ASSERT(isBaselineJS());
  return (BaselineFrame*)(fp() - BaselineFrame::Size());
}

inline uint32_t JSJitFrameIter::baselineFrameNumValueSlots() const {
  MOZ_ASSERT(isBaselineJS());
  return baselineFrame()->numValueSlots(*baselineFrameSize_);
}

template <typename T>
bool JSJitFrameIter::isExitFrameLayout() const {
  if (!isExitFrame()) {
    return false;
  }
  return exitFrame()->is<T>();
}

}  // namespace jit
}  // namespace js

#endif /* jit_JSJitFrameIter_inl_h */
