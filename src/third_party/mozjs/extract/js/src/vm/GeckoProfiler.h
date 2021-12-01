/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GeckoProfiler_h
#define vm_GeckoProfiler_h

#include "mozilla/DebugOnly.h"
#include "mozilla/GuardObjects.h"

#include <stddef.h>

#include "js/ProfilingStack.h"
#include "threading/ExclusiveData.h"
#include "vm/JSScript.h"
#include "vm/MutexIDs.h"

/*
 * Gecko Profiler integration with the JS Engine
 * https://developer.mozilla.org/en/Performance/Profiling_with_the_Built-in_Profiler
 *
 * The Gecko Profiler (found in tools/profiler) is an implementation of a
 * profiler which has the ability to walk the C++ stack as well as use
 * instrumentation to gather information. When dealing with JS, however, the
 * profiler needs integration with the engine because otherwise it is very
 * difficult to figure out what javascript is executing.
 *
 * The current method of integration with the profiler is a form of
 * instrumentation: every time a JS function is entered, a bit of information
 * is pushed onto a stack that the profiler owns and maintains. This
 * information is then popped at the end of the JS function. The profiler
 * informs the JS engine of this stack at runtime, and it can by turned on/off
 * dynamically. Each stack entry has type ProfileEntry.
 *
 * Throughout execution, the size of the stack recorded in memory may exceed the
 * maximum. The JS engine will not write any information past the maximum limit,
 * but it will still maintain the size of the stack. Profiler code is aware of
 * this and iterates the stack accordingly.
 *
 * There is some information pushed on the profiler stack for every JS function
 * that is entered. First is a char* label with a description of what function
 * was entered. Currently this string is of the form "function (file:line)" if
 * there's a function name, or just "file:line" if there's no function name
 * available. The other bit of information is the relevant C++ (native) stack
 * pointer. This stack pointer is what enables the interleaving of the C++ and
 * the JS stack. Finally, throughout execution of the function, some extra
 * information may be updated on the ProfileEntry structure.
 *
 * = Profile Strings
 *
 * The profile strings' allocations and deallocation must be carefully
 * maintained, and ideally at a very low overhead cost. For this reason, the JS
 * engine maintains a mapping of all known profile strings. These strings are
 * keyed in lookup by a JSScript*, but are serialized with a JSFunction*,
 * JSScript* pair. A JSScript will destroy its corresponding profile string when
 * the script is finalized.
 *
 * For this reason, a char* pointer pushed on the profiler stack is valid only
 * while it is on the profiler stack. The profiler uses sampling to read off
 * information from this instrumented stack, and it therefore copies the string
 * byte for byte when a JS function is encountered during sampling.
 *
 * = Native Stack Pointer
 *
 * The actual value pushed as the native pointer is nullptr for most JS
 * functions. The reason for this is that there's actually very little
 * correlation between the JS stack and the C++ stack because many JS functions
 * all run in the same C++ frame, or can even go backwards in C++ when going
 * from the JIT back to the interpreter.
 *
 * To alleviate this problem, all JS functions push nullptr as their "native
 * stack pointer" to indicate that it's a JS function call. The function
 * RunScript(), however, pushes an actual C++ stack pointer onto the profiler
 * stack. This way when interleaving C++ and JS, if the Gecko Profiler sees a
 * nullptr native stack pointer on the profiler stack, it looks backwards for
 * the first non-nullptr pointer and uses that for all subsequent nullptr
 * native stack pointers.
 *
 * = Line Numbers
 *
 * One goal of sampling is to get both a backtrace of the JS stack, but also
 * know where within each function on the stack execution currently is. For
 * this, each ProfileEntry has a 'pc' field to tell where its execution
 * currently is. This field is updated whenever a call is made to another JS
 * function, and for the JIT it is also updated whenever the JIT is left.
 *
 * This field is in a union with a uint32_t 'line' so that C++ can make use of
 * the field as well. It was observed that tracking 'line' via PCToLineNumber in
 * JS was far too expensive, so that is why the pc instead of the translated
 * line number is stored.
 *
 * As an invariant, if the pc is nullptr, then the JIT is currently executing
 * generated code. Otherwise execution is in another JS function or in C++. With
 * this in place, only the top entry of the stack can ever have nullptr as its
 * pc. Additionally with this invariant, it is possible to maintain mappings of
 * JIT code to pc which can be accessed safely because they will only be
 * accessed from a signal handler when the JIT code is executing.
 */

namespace js {

// The `ProfileStringMap` weakly holds its `JSScript*` keys and owns its string
// values. Entries are removed when the `JSScript` is finalized; see
// `GeckoProfiler::onScriptFinalized`.
using ProfileStringMap = HashMap<JSScript*,
                                 UniqueChars,
                                 DefaultHasher<JSScript*>,
                                 SystemAllocPolicy>;

class GeckoProfilerRuntime
{
    JSRuntime*           rt;
    ExclusiveData<ProfileStringMap> strings;
    bool                 slowAssertions;
    uint32_t             enabled_;
    void                (*eventMarker_)(const char*);

    UniqueChars allocProfileString(JSScript* script, JSFunction* function);

  public:
    explicit GeckoProfilerRuntime(JSRuntime* rt);

    bool init();

    /* management of whether instrumentation is on or off */
    bool enabled() { return enabled_; }
    void enable(bool enabled);
    void enableSlowAssertions(bool enabled) { slowAssertions = enabled; }
    bool slowAssertionsEnabled() { return slowAssertions; }

    void setEventMarker(void (*fn)(const char*));
    const char* profileString(JSScript* script, JSFunction* maybeFun);
    void onScriptFinalized(JSScript* script);

    void markEvent(const char* event);

    /* meant to be used for testing, not recommended to call in normal code */
    size_t stringsCount();
    void stringsReset();

    uint32_t* addressOfEnabled() {
        return &enabled_;
    }

    void fixupStringsMapAfterMovingGC();
#ifdef JSGC_HASH_TABLE_CHECKS
    void checkStringsMapAfterMovingGC();
#endif
};

inline size_t
GeckoProfilerRuntime::stringsCount()
{
    return strings.lock()->count();
}

inline void
GeckoProfilerRuntime::stringsReset()
{
    strings.lock()->clear();
}

/*
 * This class is used in RunScript() to push the marker onto the sampling stack
 * that we're about to enter JS function calls. This is the only time in which a
 * valid stack pointer is pushed to the sampling stack.
 */
class MOZ_RAII GeckoProfilerEntryMarker
{
  public:
    explicit MOZ_ALWAYS_INLINE
    GeckoProfilerEntryMarker(JSContext* cx,
                             JSScript* script
                             MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    MOZ_ALWAYS_INLINE ~GeckoProfilerEntryMarker();

  private:
    GeckoProfilerThread* profiler_;
#ifdef DEBUG
    uint32_t spBefore_;
#endif
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * RAII class to automatically add Gecko Profiler pseudo frame entries.
 *
 * NB: The `label` string must be statically allocated.
 */
class MOZ_NONHEAP_CLASS AutoGeckoProfilerEntry
{
  public:
    explicit MOZ_ALWAYS_INLINE
    AutoGeckoProfilerEntry(JSContext* cx, const char* label,
                           ProfileEntry::Category category = ProfileEntry::Category::JS
                           MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    MOZ_ALWAYS_INLINE ~AutoGeckoProfilerEntry();

  private:
    GeckoProfilerThread* profiler_;
#ifdef DEBUG
    uint32_t spBefore_;
#endif
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * This class is used in the interpreter to bound regions where the baseline JIT
 * being entered via OSR.  It marks the current top pseudostack entry as
 * OSR-ed
 */
class MOZ_RAII GeckoProfilerBaselineOSRMarker
{
  public:
    explicit GeckoProfilerBaselineOSRMarker(JSContext* cx, bool hasProfilerFrame
                                            MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    ~GeckoProfilerBaselineOSRMarker();

  private:
    GeckoProfilerThread* profiler;
    mozilla::DebugOnly<uint32_t> spBefore_;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * This class manages the instrumentation portion of the profiling for JIT
 * code.
 *
 * The instrumentation tracks entry into functions, leaving those functions via
 * a function call, reentering the functions from a function call, and exiting
 * the functions from returning. This class also handles inline frames and
 * manages the instrumentation which needs to be attached to them as well.
 *
 * The basic methods which emit instrumentation are at the end of this class,
 * and the management functions are all described in the middle.
 */
template<class Assembler, class Register>
class GeckoProfilerInstrumentation
{
    GeckoProfilerRuntime* profiler_; // Instrumentation location management

  public:
    /*
     * Creates instrumentation which writes information out the the specified
     * profiler's stack and constituent fields.
     */
    explicit GeckoProfilerInstrumentation(GeckoProfilerRuntime* profiler) : profiler_(profiler) {}

    /* Small proxies around GeckoProfiler */
    bool enabled() { return profiler_ && profiler_->enabled(); }
    GeckoProfilerRuntime* profiler() { MOZ_ASSERT(enabled()); return profiler_; }
    void disable() { profiler_ = nullptr; }
};

} /* namespace js */

#endif /* vm_GeckoProfiler_h */
