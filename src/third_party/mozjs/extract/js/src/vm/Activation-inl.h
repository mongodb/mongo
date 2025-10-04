/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Activation_inl_h
#define vm_Activation_inl_h

#include "vm/Activation.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}, MOZ_CRASH
#include "mozilla/Likely.h"      // MOZ_UNLIKELY
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include "jit/CalleeToken.h"   // js::jit::CalleeToken
#include "js/Debug.h"          // JS::dbg::AutoEntryMonitor
#include "vm/FrameIter.h"      // js::FrameIter
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"      // JSContext
#include "vm/Stack.h"          // js::AbstractFramePtr

namespace js {

inline ActivationEntryMonitor::ActivationEntryMonitor(JSContext* cx)
    : cx_(cx), entryMonitor_(cx->entryMonitor) {
  cx->entryMonitor = nullptr;
}

inline ActivationEntryMonitor::ActivationEntryMonitor(
    JSContext* cx, InterpreterFrame* entryFrame)
    : ActivationEntryMonitor(cx) {
  if (MOZ_UNLIKELY(entryMonitor_)) {
    init(cx, entryFrame);
  }
}

inline ActivationEntryMonitor::ActivationEntryMonitor(
    JSContext* cx, jit::CalleeToken entryToken)
    : ActivationEntryMonitor(cx) {
  if (MOZ_UNLIKELY(entryMonitor_)) {
    init(cx, entryToken);
  }
}

inline ActivationEntryMonitor::~ActivationEntryMonitor() {
  if (entryMonitor_) {
    entryMonitor_->Exit(cx_);
  }

  cx_->entryMonitor = entryMonitor_;
}

inline Activation::Activation(JSContext* cx, Kind kind)
    : cx_(cx),
      compartment_(cx->compartment()),
      prev_(cx->activation_),
      prevProfiling_(prev_ ? prev_->mostRecentProfiling() : nullptr),
      hideScriptedCallerCount_(0),
      frameCache_(cx),
      asyncStack_(cx, cx->asyncStackForNewActivations()),
      asyncCause_(cx->asyncCauseForNewActivations),
      asyncCallIsExplicit_(cx->asyncCallIsExplicit),
      kind_(kind) {
  cx->asyncStackForNewActivations() = nullptr;
  cx->asyncCauseForNewActivations = nullptr;
  cx->asyncCallIsExplicit = false;
  cx->activation_ = this;
}

inline Activation::~Activation() {
  MOZ_ASSERT_IF(isProfiling(), this != cx_->profilingActivation_);
  MOZ_ASSERT(cx_->activation_ == this);
  MOZ_ASSERT(hideScriptedCallerCount_ == 0);
  cx_->activation_ = prev_;
  cx_->asyncCauseForNewActivations = asyncCause_;
  cx_->asyncStackForNewActivations() = asyncStack_;
  cx_->asyncCallIsExplicit = asyncCallIsExplicit_;
}

inline bool Activation::isProfiling() const {
  if (isInterpreter()) {
    return asInterpreter()->isProfiling();
  }

  MOZ_ASSERT(isJit());
  return asJit()->isProfiling();
}

inline Activation* Activation::mostRecentProfiling() {
  if (isProfiling()) {
    return this;
  }
  return prevProfiling_;
}

inline LiveSavedFrameCache* Activation::getLiveSavedFrameCache(JSContext* cx) {
  if (!frameCache_.get().initialized() && !frameCache_.get().init(cx)) {
    return nullptr;
  }
  return frameCache_.address();
}

/* static */ inline mozilla::Maybe<LiveSavedFrameCache::FramePtr>
LiveSavedFrameCache::FramePtr::create(const FrameIter& iter) {
  if (iter.done()) {
    return mozilla::Nothing();
  }

  if (iter.isPhysicalJitFrame()) {
    return mozilla::Some(FramePtr(iter.physicalJitFrame()));
  }

  if (!iter.hasUsableAbstractFramePtr()) {
    return mozilla::Nothing();
  }

  auto afp = iter.abstractFramePtr();

  if (afp.isInterpreterFrame()) {
    return mozilla::Some(FramePtr(afp.asInterpreterFrame()));
  }
  if (afp.isWasmDebugFrame()) {
    return mozilla::Some(FramePtr(afp.asWasmDebugFrame()));
  }
  if (afp.isRematerializedFrame()) {
    return mozilla::Some(FramePtr(afp.asRematerializedFrame()));
  }

  MOZ_CRASH("unexpected frame type");
}

struct LiveSavedFrameCache::FramePtr::HasCachedMatcher {
  template <typename Frame>
  bool operator()(Frame* f) const {
    return f->hasCachedSavedFrame();
  }
};

inline bool LiveSavedFrameCache::FramePtr::hasCachedSavedFrame() const {
  return ptr.match(HasCachedMatcher());
}

struct LiveSavedFrameCache::FramePtr::SetHasCachedMatcher {
  template <typename Frame>
  void operator()(Frame* f) {
    f->setHasCachedSavedFrame();
  }
};

inline void LiveSavedFrameCache::FramePtr::setHasCachedSavedFrame() {
  ptr.match(SetHasCachedMatcher());
}

struct LiveSavedFrameCache::FramePtr::ClearHasCachedMatcher {
  template <typename Frame>
  void operator()(Frame* f) {
    f->clearHasCachedSavedFrame();
  }
};

inline void LiveSavedFrameCache::FramePtr::clearHasCachedSavedFrame() {
  ptr.match(ClearHasCachedMatcher());
}

inline bool Activation::hasWasmExitFP() const {
  return isJit() && asJit()->hasWasmExitFP();
}

}  // namespace js

#endif  // vm_Activation_inl_h
