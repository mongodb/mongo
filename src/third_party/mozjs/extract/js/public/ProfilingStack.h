/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProfilingStack_h
#define js_ProfilingStack_h

#include <algorithm>
#include <stdint.h>

#include "jstypes.h"

#include "js/TypeDecls.h"
#include "js/Utility.h"

class JSTracer;
class PseudoStack;

// This file defines the classes PseudoStack and ProfileEntry.
// The PseudoStack manages an array of ProfileEntries.
// Usage:
//
//  PseudoStack* pseudoStack = ...;
//
//  // For CPP stack frames:
//  pseudoStack->pushCppFrame(...);
//  // Execute some code. When finished, pop the entry:
//  pseudoStack->pop();
//
//  // For JS stack frames:
//  pseudoStack->pushJSFrame(...);
//  // Execute some code. When finished, pop the entry:
//  pseudoStack->pop();
//
//
// Concurrency considerations
//
// A thread's pseudo stack (and the entries inside it) is only modified by
// that thread. However, the pseudo stack can be *read* by a different thread,
// the sampler thread: Whenever the profiler wants to sample a given thread A,
// the following happens:
//  (1) Thread A is suspended.
//  (2) The sampler thread (thread S) reads the PseudoStack of thread A,
//      including all ProfileEntries that are currently in that stack
//      (pseudoStack->entries[0..pseudoStack->stackSize()]).
//  (3) Thread A is resumed.
//
// Thread suspension is achieved using platform-specific APIs; refer to each
// platform's Sampler::SuspendAndSampleAndResumeThread implementation in
// platform-*.cpp for details.
//
// When the thread is suspended, the values in pseudoStack->stackPointer and in
// the entry range pseudoStack->entries[0..pseudoStack->stackPointer] need to
// be in a consistent state, so that thread S does not read partially-
// constructed profile entries. More specifically, we have two requirements:
//  (1) When adding a new entry at the top of the stack, its ProfileEntry data
//      needs to be put in place *before* the stackPointer is incremented, and
//      the compiler + CPU need to know that this order matters.
//  (2) When popping an entry from the stack and then preparing the
//      ProfileEntry data for the next frame that is about to be pushed, the
//      decrement of the stackPointer in pop() needs to happen *before* the
//      ProfileEntry for the new frame is being popuplated, and the compiler +
//      CPU need to know that this order matters.
//
// We can express the relevance of these orderings in multiple ways.
// Option A is to make stackPointer an atomic with SequentiallyConsistent
// memory ordering. This would ensure that no writes in thread A would be
// reordered across any writes to stackPointer, which satisfies requirements
// (1) and (2) at the same time. Option A is the simplest.
// Option B is to use ReleaseAcquire memory ordering both for writes to
// stackPointer *and* for writes to ProfileEntry fields. Release-stores ensure
// that all writes that happened *before this write in program order* are not
// reordered to happen after this write. ReleaseAcquire ordering places no
// requirements on the ordering of writes that happen *after* this write in
// program order.
// Using release-stores for writes to stackPointer expresses requirement (1),
// and using release-stores for writes to the ProfileEntry fields expresses
// requirement (2).
//
// Option B is more complicated than option A, but has much better performance
// on x86/64: In a microbenchmark run on a Macbook Pro from 2017, switching
// from option A to option B reduced the overhead of pushing+popping a
// ProfileEntry by 10 nanoseconds.
// On x86/64, release-stores require no explicit hardware barriers or lock
// instructions.
// On ARM/64, option B may be slower than option A, because the compiler will
// generate hardware barriers for every single release-store instead of just
// for the writes to stackPointer. However, the actual performance impact of
// this has not yet been measured on ARM, so we're currently using option B
// everywhere. This is something that we may want to change in the future once
// we've done measurements.

namespace js {

// A call stack can be specified to the JS engine such that all JS entry/exits
// to functions push/pop an entry to/from the specified stack.
//
// For more detailed information, see vm/GeckoProfiler.h.
//
class ProfileEntry
{
    // A ProfileEntry represents either a C++ profile entry or a JS one.

    // WARNING WARNING WARNING
    //
    // All the fields below are Atomic<...,ReleaseAcquire>. This is needed so
    // that writes to these fields are release-writes, which ensures that
    // earlier writes in this thread don't get reordered after the writes to
    // these fields. In particular, the decrement of the stack pointer in
    // PseudoStack::pop() is a write that *must* happen before the values in
    // this ProfileEntry are changed. Otherwise, the sampler thread might see
    // an inconsistent state where the stack pointer still points to a
    // ProfileEntry which has already been popped off the stack and whose
    // fields have now been partially repopulated with new values.
    // See the "Concurrency considerations" paragraph at the top of this file
    // for more details.

    // Descriptive label for this entry. Must be a static string! Can be an
    // empty string, but not a null pointer.
    mozilla::Atomic<const char*, mozilla::ReleaseAcquire> label_;

    // An additional descriptive string of this entry which is combined with
    // |label_| in profiler output. Need not be (and usually isn't) static. Can
    // be null.
    mozilla::Atomic<const char*, mozilla::ReleaseAcquire> dynamicString_;

    // Stack pointer for non-JS entries, the script pointer otherwise.
    mozilla::Atomic<void*, mozilla::ReleaseAcquire> spOrScript;

    // Line number for non-JS entries, the bytecode offset otherwise.
    mozilla::Atomic<int32_t, mozilla::ReleaseAcquire> lineOrPcOffset;

    // Bits 0...1 hold the Kind. Bits 2...3 are unused. Bits 4...12 hold the
    // Category.
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> kindAndCategory_;

    static int32_t pcToOffset(JSScript* aScript, jsbytecode* aPc);

  public:
    enum class Kind : uint32_t {
        // A normal C++ frame.
        CPP_NORMAL = 0,

        // A special C++ frame indicating the start of a run of JS pseudostack
        // entries. CPP_MARKER_FOR_JS frames are ignored, except for the sp
        // field.
        CPP_MARKER_FOR_JS = 1,

        // A normal JS frame.
        JS_NORMAL = 2,

        // An interpreter JS frame that has OSR-ed into baseline. JS_NORMAL
        // frames can be converted to JS_OSR and back. JS_OSR frames are
        // ignored.
        JS_OSR = 3,

        KIND_MASK = 0x3,
    };

    // Keep these in sync with devtools/client/performance/modules/categories.js
    enum class Category : uint32_t {
        OTHER    = 1u << 4,
        CSS      = 1u << 5,
        JS       = 1u << 6,
        GC       = 1u << 7,
        CC       = 1u << 8,
        NETWORK  = 1u << 9,
        GRAPHICS = 1u << 10,
        STORAGE  = 1u << 11,
        EVENTS   = 1u << 12,

        FIRST    = OTHER,
        LAST     = EVENTS,

        CATEGORY_MASK = ~uint32_t(Kind::KIND_MASK),
    };

    static_assert((uint32_t(Category::FIRST) & uint32_t(Kind::KIND_MASK)) == 0,
                  "Category overlaps with Kind");

    bool isCpp() const
    {
        Kind k = kind();
        return k == Kind::CPP_NORMAL || k == Kind::CPP_MARKER_FOR_JS;
    }

    bool isJs() const
    {
        Kind k = kind();
        return k == Kind::JS_NORMAL || k == Kind::JS_OSR;
    }

    void setLabel(const char* aLabel) { label_ = aLabel; }
    const char* label() const { return label_; }

    const char* dynamicString() const { return dynamicString_; }

    void initCppFrame(const char* aLabel, const char* aDynamicString, void* sp, uint32_t aLine,
                      Kind aKind, Category aCategory)
    {
        label_ = aLabel;
        dynamicString_ = aDynamicString;
        spOrScript = sp;
        lineOrPcOffset = static_cast<int32_t>(aLine);
        kindAndCategory_ = uint32_t(aKind) | uint32_t(aCategory);
        MOZ_ASSERT(isCpp());
    }

    void initJsFrame(const char* aLabel, const char* aDynamicString, JSScript* aScript,
                     jsbytecode* aPc)
    {
        label_ = aLabel;
        dynamicString_ = aDynamicString;
        spOrScript = aScript;
        lineOrPcOffset = pcToOffset(aScript, aPc);
        kindAndCategory_ = uint32_t(Kind::JS_NORMAL) | uint32_t(Category::JS);
        MOZ_ASSERT(isJs());
    }

    void setKind(Kind aKind) {
        kindAndCategory_ = uint32_t(aKind) | uint32_t(category());
    }

    Kind kind() const {
        return Kind(kindAndCategory_ & uint32_t(Kind::KIND_MASK));
    }

    Category category() const {
        return Category(kindAndCategory_ & uint32_t(Category::CATEGORY_MASK));
    }

    void* stackAddress() const {
        MOZ_ASSERT(!isJs());
        return spOrScript;
    }

    JS_PUBLIC_API(JSScript*) script() const;

    uint32_t line() const {
        MOZ_ASSERT(!isJs());
        return static_cast<uint32_t>(lineOrPcOffset);
    }

    // Note that the pointer returned might be invalid.
    JSScript* rawScript() const {
        MOZ_ASSERT(isJs());
        void* script = spOrScript;
        return static_cast<JSScript*>(script);
    }

    // We can't know the layout of JSScript, so look in vm/GeckoProfiler.cpp.
    JS_FRIEND_API(jsbytecode*) pc() const;
    void setPC(jsbytecode* pc);

    void trace(JSTracer* trc);

    // The offset of a pc into a script's code can actually be 0, so to
    // signify a nullptr pc, use a -1 index. This is checked against in
    // pc() and setPC() to set/get the right pc.
    static const int32_t NullPCOffset = -1;
};

JS_FRIEND_API(void)
SetContextProfilingStack(JSContext* cx, PseudoStack* pseudoStack);

// GetContextProfilingStack also exists, but it's defined in RootingAPI.h.

JS_FRIEND_API(void)
EnableContextProfilingStack(JSContext* cx, bool enabled);

JS_FRIEND_API(void)
RegisterContextProfilingEventMarker(JSContext* cx, void (*fn)(const char*));

} // namespace js

// Each thread has its own PseudoStack. That thread modifies the PseudoStack,
// pushing and popping elements as necessary.
//
// The PseudoStack is also read periodically by the profiler's sampler thread.
// This happens only when the thread that owns the PseudoStack is suspended. So
// there are no genuine parallel accesses.
//
// However, it is possible for pushing/popping to be interrupted by a periodic
// sample. Because of this, we need pushing/popping to be effectively atomic.
//
// - When pushing a new entry, we increment the stack pointer -- making the new
//   entry visible to the sampler thread -- only after the new entry has been
//   fully written. The stack pointer is Atomic<uint32_t,ReleaseAcquire>, so
//   the increment is a release-store, which ensures that this store is not
//   reordered before the writes of the entry.
//
// - When popping an old entry, the only operation is the decrementing of the
//   stack pointer, which is obviously atomic.
//
class PseudoStack final
{
  public:
    PseudoStack()
      : stackPointer(0)
    {}

    ~PseudoStack() {
        // The label macros keep a reference to the PseudoStack to avoid a TLS
        // access. If these are somehow not all cleared we will get a
        // use-after-free so better to crash now.
        MOZ_RELEASE_ASSERT(stackPointer == 0);
    }

    void pushCppFrame(const char* label, const char* dynamicString, void* sp, uint32_t line,
                      js::ProfileEntry::Kind kind, js::ProfileEntry::Category category) {
        if (stackPointer < MaxEntries) {
            entries[stackPointer].initCppFrame(label, dynamicString, sp, line, kind, category);
        }

        // This must happen at the end! The compiler will not reorder this
        // update because stackPointer is Atomic<..., ReleaseAcquire>, so any
        // the writes above will not be reordered below the stackPointer store.
        // Do the read and the write as two separate statements, in order to
        // make it clear that we don't need an atomic increment, which would be
        // more expensive on x86 than the separate operations done here.
        // This thread is the only one that ever changes the value of
        // stackPointer.
        uint32_t oldStackPointer = stackPointer;
        stackPointer = oldStackPointer + 1;
    }

    void pushJsFrame(const char* label, const char* dynamicString, JSScript* script,
                     jsbytecode* pc) {
        if (stackPointer < MaxEntries) {
            entries[stackPointer].initJsFrame(label, dynamicString, script, pc);
        }

        // This must happen at the end! The compiler will not reorder this
        // update because stackPointer is Atomic<..., ReleaseAcquire>, which
        // makes this assignment a release-store, so the writes above will not
        // be reordered to occur after the stackPointer store.
        // Do the read and the write as two separate statements, in order to
        // make it clear that we don't need an atomic increment, which would be
        // more expensive on x86 than the separate operations done here.
        // This thread is the only one that ever changes the value of
        // stackPointer.
        uint32_t oldStackPointer = stackPointer;
        stackPointer = oldStackPointer + 1;
    }

    void pop() {
        MOZ_ASSERT(stackPointer > 0);
        // Do the read and the write as two separate statements, in order to
        // make it clear that we don't need an atomic decrement, which would be
        // more expensive on x86 than the separate operations done here.
        // This thread is the only one that ever changes the value of
        // stackPointer.
        uint32_t oldStackPointer = stackPointer;
        stackPointer = oldStackPointer - 1;
    }

    uint32_t stackSize() const { return std::min(uint32_t(stackPointer), uint32_t(MaxEntries)); }

  private:
    // No copying.
    PseudoStack(const PseudoStack&) = delete;
    void operator=(const PseudoStack&) = delete;

  public:
    static const uint32_t MaxEntries = 1024;

    // The stack entries.
    js::ProfileEntry entries[MaxEntries];

    // This may exceed MaxEntries, so instead use the stackSize() method to
    // determine the number of valid samples in entries. When this is less
    // than MaxEntries, it refers to the first free entry past the top of the
    // in-use stack (i.e. entries[stackPointer - 1] is the top stack entry).
    //
    // WARNING WARNING WARNING
    //
    // This is an atomic variable that uses ReleaseAcquire memory ordering.
    // See the "Concurrency considerations" paragraph at the top of this file
    // for more details.
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> stackPointer;
};

namespace js {

class AutoGeckoProfilerEntry;
class GeckoProfilerEntryMarker;
class GeckoProfilerBaselineOSRMarker;

class GeckoProfilerThread
{
    friend class AutoGeckoProfilerEntry;
    friend class GeckoProfilerEntryMarker;
    friend class GeckoProfilerBaselineOSRMarker;

    PseudoStack*         pseudoStack_;

  public:
    GeckoProfilerThread();

    uint32_t stackPointer() { MOZ_ASSERT(installed()); return pseudoStack_->stackPointer; }
    ProfileEntry* stack() { return pseudoStack_->entries; }
    PseudoStack* getPseudoStack() { return pseudoStack_; }

    /* management of whether instrumentation is on or off */
    bool installed() { return pseudoStack_ != nullptr; }

    void setProfilingStack(PseudoStack* pseudoStack);
    void trace(JSTracer* trc);

    /*
     * Functions which are the actual instrumentation to track run information
     *
     *   - enter: a function has started to execute
     *   - updatePC: updates the pc information about where a function
     *               is currently executing
     *   - exit: this function has ceased execution, and no further
     *           entries/exits will be made
     */
    bool enter(JSContext* cx, JSScript* script, JSFunction* maybeFun);
    void exit(JSScript* script, JSFunction* maybeFun);
    inline void updatePC(JSContext* cx, JSScript* script, jsbytecode* pc);
};

} // namespace js

#endif  /* js_ProfilingStack_h */
