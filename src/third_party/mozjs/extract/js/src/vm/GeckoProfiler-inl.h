/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GeckoProfiler_inl_h
#define vm_GeckoProfiler_inl_h

#include "vm/GeckoProfiler.h"

#include "js/ProfilingStack.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/Runtime.h"

namespace js {

inline void GeckoProfilerThread::updatePC(JSContext* cx, JSScript* script,
                                          jsbytecode* pc) {
  if (!cx->runtime()->geckoProfiler().enabled()) {
    return;
  }

  uint32_t sp = profilingStack_->stackPointer;
  if (sp - 1 < profilingStack_->stackCapacity()) {
    MOZ_ASSERT(sp > 0);
    MOZ_ASSERT(profilingStack_->frames[sp - 1].rawScript() == script);
    profilingStack_->frames[sp - 1].setPC(pc);
  }
}

/*
 * This class is used to suppress profiler sampling during
 * critical sections where stack state is not valid.
 */
class MOZ_RAII AutoSuppressProfilerSampling {
 public:
  explicit AutoSuppressProfilerSampling(JSContext* cx);

  ~AutoSuppressProfilerSampling();

 private:
  JSContext* cx_;
  bool previouslyEnabled_;
};

MOZ_ALWAYS_INLINE
GeckoProfilerEntryMarker::GeckoProfilerEntryMarker(JSContext* cx,
                                                   JSScript* script)
    : profiler_(&cx->geckoProfiler()) {
  if (MOZ_LIKELY(!profiler_->infraInstalled())) {
    profiler_ = nullptr;
#ifdef DEBUG
    spBefore_ = 0;
#endif
    return;
  }
#ifdef DEBUG
  spBefore_ = profiler_->stackPointer();
#endif

  // Push an sp marker frame so the profiler can correctly order JS and native
  // stacks.
  profiler_->profilingStack_->pushSpMarkerFrame(this);

  profiler_->profilingStack_->pushJsFrame(
      "js::RunScript",
      /* dynamicString = */ nullptr, script, script->code(),
      script->realm()->creationOptions().profilerRealmID());
}

MOZ_ALWAYS_INLINE
GeckoProfilerEntryMarker::~GeckoProfilerEntryMarker() {
  if (MOZ_LIKELY(profiler_ == nullptr)) {
    return;
  }

  profiler_->profilingStack_->pop();  // the JS frame
  profiler_->profilingStack_->pop();  // the SP_MARKER frame
  MOZ_ASSERT(spBefore_ == profiler_->stackPointer());
}

MOZ_ALWAYS_INLINE
AutoGeckoProfilerEntry::AutoGeckoProfilerEntry(
    JSContext* cx, const char* label, const char* dynamicString,
    JS::ProfilingCategoryPair categoryPair, uint32_t flags) {
  profilingStack_ = GetContextProfilingStackIfEnabled(cx);
  if (MOZ_LIKELY(!profilingStack_)) {
#ifdef DEBUG
    profiler_ = nullptr;
    spBefore_ = 0;
#endif
    return;
  }

#ifdef DEBUG
  profiler_ = &cx->geckoProfiler();
  spBefore_ = profiler_->stackPointer();
#endif

  profilingStack_->pushLabelFrame(label, dynamicString,
                                  /* sp = */ this, categoryPair, flags);
}

MOZ_ALWAYS_INLINE
AutoGeckoProfilerEntry::~AutoGeckoProfilerEntry() {
  if (MOZ_LIKELY(!profilingStack_)) {
    return;
  }

  profilingStack_->pop();
  MOZ_ASSERT(spBefore_ == profiler_->stackPointer());
}

MOZ_ALWAYS_INLINE
AutoGeckoProfilerEntry::AutoGeckoProfilerEntry(
    JSContext* cx, const char* label, JS::ProfilingCategoryPair categoryPair,
    uint32_t flags)
    : AutoGeckoProfilerEntry(cx, label, /* dynamicString */ nullptr,
                             categoryPair, flags) {}

MOZ_ALWAYS_INLINE
AutoJSMethodProfilerEntry::AutoJSMethodProfilerEntry(JSContext* cx,
                                                     const char* label,
                                                     const char* dynamicString)
    : AutoGeckoProfilerEntry(
          cx, label, dynamicString, JS::ProfilingCategoryPair::JS_Builtin,
          uint32_t(ProfilingStackFrame::Flags::RELEVANT_FOR_JS) |
              uint32_t(ProfilingStackFrame::Flags::STRING_TEMPLATE_METHOD)) {}

MOZ_ALWAYS_INLINE
AutoJSConstructorProfilerEntry::AutoJSConstructorProfilerEntry(
    JSContext* cx, const char* label)
    : AutoGeckoProfilerEntry(
          cx, label, "constructor", JS::ProfilingCategoryPair::JS_Builtin,
          uint32_t(ProfilingStackFrame::Flags::RELEVANT_FOR_JS)) {}

}  // namespace js

#endif  // vm_GeckoProfiler_inl_h
