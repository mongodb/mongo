/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitFrames_inl_h
#define jit_JitFrames_inl_h

#include "jit/JitFrames.h"
#include "vm/JSContext.h"

#include "jit/JSJitFrameIter-inl.h"

namespace js {
namespace jit {

inline BaselineFrame* GetTopBaselineFrame(JSContext* cx) {
  JSJitFrameIter frame(cx->activation()->asJit());
  MOZ_ASSERT(frame.type() == FrameType::Exit);
  ++frame;
  if (frame.isBaselineStub()) {
    ++frame;
  }
  MOZ_ASSERT(frame.isBaselineJS());
  return frame.baselineFrame();
}

}  // namespace jit
}  // namespace js

#endif /* jit_JitFrames_inl_h */
