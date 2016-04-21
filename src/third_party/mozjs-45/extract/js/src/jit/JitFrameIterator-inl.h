/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitFrameIterator_inl_h
#define jit_JitFrameIterator_inl_h

#include "jit/JitFrameIterator.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"

namespace js {
namespace jit {

inline JitFrameLayout*
JitProfilingFrameIterator::framePtr()
{
    MOZ_ASSERT(!done());
    return (JitFrameLayout*) fp_;
}

inline JSScript*
JitProfilingFrameIterator::frameScript()
{
    return ScriptFromCalleeToken(framePtr()->calleeToken());
}

inline BaselineFrame*
JitFrameIterator::baselineFrame() const
{
    MOZ_ASSERT(isBaselineJS());
    return (BaselineFrame*)(fp() - BaselineFrame::FramePointerOffset - BaselineFrame::Size());
}

template <typename T>
bool
JitFrameIterator::isExitFrameLayout() const
{
    if (!isExitFrame() || isFakeExitFrame())
        return false;
    return exitFrame()->is<T>();
}

} // namespace jit
} // namespace js

#endif /* jit_JitFrameIterator_inl_h */
