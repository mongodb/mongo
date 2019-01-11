/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GeckoProfiler_inl_h
#define vm_GeckoProfiler_inl_h

#include "vm/GeckoProfiler.h"

#include "vm/JSContext.h"
#include "vm/Runtime.h"

namespace js {

inline void
GeckoProfilerThread::updatePC(JSContext* cx, JSScript* script, jsbytecode* pc)
{
    if (!cx->runtime()->geckoProfiler().enabled())
        return;

    uint32_t sp = pseudoStack_->stackPointer;
    if (sp - 1 < PseudoStack::MaxEntries) {
        MOZ_ASSERT(sp > 0);
        MOZ_ASSERT(pseudoStack_->entries[sp - 1].rawScript() == script);
        pseudoStack_->entries[sp - 1].setPC(pc);
    }
}

/*
 * This class is used to suppress profiler sampling during
 * critical sections where stack state is not valid.
 */
class MOZ_RAII AutoSuppressProfilerSampling
{
  public:
    explicit AutoSuppressProfilerSampling(JSContext* cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

    ~AutoSuppressProfilerSampling();

  private:
    JSContext* cx_;
    bool previouslyEnabled_;
    JSRuntime::AutoProhibitActiveContextChange prohibitContextChange_;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

MOZ_ALWAYS_INLINE
GeckoProfilerEntryMarker::GeckoProfilerEntryMarker(JSContext* cx,
                                                   JSScript* script
                                                   MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : profiler_(&cx->geckoProfiler())
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    if (MOZ_LIKELY(!profiler_->installed())) {
        profiler_ = nullptr;
        return;
    }
#ifdef DEBUG
    spBefore_ = profiler_->stackPointer();
#endif

    // We want to push a CPP frame so the profiler can correctly order JS and native stacks.
    // Only the sp value is important.
    profiler_->pseudoStack_->pushCppFrame(
        /* label = */ "", /* dynamicString = */ nullptr, /* sp = */ this, /* line = */ 0,
        ProfileEntry::Kind::CPP_MARKER_FOR_JS, ProfileEntry::Category::OTHER);

    profiler_->pseudoStack_->pushJsFrame(
        "js::RunScript", /* dynamicString = */ nullptr, script, script->code());
}

MOZ_ALWAYS_INLINE
GeckoProfilerEntryMarker::~GeckoProfilerEntryMarker()
{
    if (MOZ_LIKELY(profiler_ == nullptr))
        return;

    profiler_->pseudoStack_->pop();    // the JS frame
    profiler_->pseudoStack_->pop();    // the BEGIN_PSEUDO_JS frame
    MOZ_ASSERT(spBefore_ == profiler_->stackPointer());
}

MOZ_ALWAYS_INLINE
AutoGeckoProfilerEntry::AutoGeckoProfilerEntry(JSContext* cx, const char* label,
                                               ProfileEntry::Category category
                                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : profiler_(&cx->geckoProfiler())
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    if (MOZ_LIKELY(!profiler_->installed())) {
        profiler_ = nullptr;
        return;
    }
#ifdef DEBUG
    spBefore_ = profiler_->stackPointer();
#endif
    profiler_->pseudoStack_->pushCppFrame(label,
                                          /* dynamicString = */ nullptr,
                                          /* sp = */ this,
                                          /* line = */ 0,
                                          ProfileEntry::Kind::CPP_NORMAL,
                                          category);
}

MOZ_ALWAYS_INLINE
AutoGeckoProfilerEntry::~AutoGeckoProfilerEntry()
{
    if (MOZ_LIKELY(!profiler_))
        return;

    profiler_->pseudoStack_->pop();
    MOZ_ASSERT(spBefore_ == profiler_->stackPointer());
}

} // namespace js

#endif // vm_GeckoProfiler_inl_h
