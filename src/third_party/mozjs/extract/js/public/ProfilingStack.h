/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProfilingStack_h
#define js_ProfilingStack_h

#include "mozilla/Atomics.h"

#include <stdint.h>

#include "jstypes.h"

#include "js/ProfilingCategory.h"
#include "js/TypeDecls.h"

class JS_PUBLIC_API JSTracer;
class JS_PUBLIC_API ProfilingStack;

// This file defines the classes ProfilingStack and ProfilingStackFrame.
// The ProfilingStack manages an array of ProfilingStackFrames.
// It keeps track of the "label stack" and the JS interpreter stack.
// The two stack types are interleaved.
//
// Usage:
//
//  ProfilingStack* profilingStack = ...;
//
//  // For label frames:
//  profilingStack->pushLabelFrame(...);
//  // Execute some code. When finished, pop the frame:
//  profilingStack->pop();
//
//  // For JS stack frames:
//  profilingStack->pushJSFrame(...);
//  // Execute some code. When finished, pop the frame:
//  profilingStack->pop();
//
//
// Concurrency considerations
//
// A thread's profiling stack (and the frames inside it) is only modified by
// that thread. However, the profiling stack can be *read* by a different
// thread, the sampler thread: Whenever the profiler wants to sample a given
// thread A, the following happens:
//  (1) Thread A is suspended.
//  (2) The sampler thread (thread S) reads the ProfilingStack of thread A,
//      including all ProfilingStackFrames that are currently in that stack
//      (profilingStack->frames[0..profilingStack->stackSize()]).
//  (3) Thread A is resumed.
//
// Thread suspension is achieved using platform-specific APIs; refer to each
// platform's Sampler::SuspendAndSampleAndResumeThread implementation in
// platform-*.cpp for details.
//
// When the thread is suspended, the values in profilingStack->stackPointer and
// in the stack frame range
// profilingStack->frames[0..profilingStack->stackPointer] need to be in a
// consistent state, so that thread S does not read partially- constructed stack
// frames. More specifically, we have two requirements:
//  (1) When adding a new frame at the top of the stack, its ProfilingStackFrame
//      data needs to be put in place *before* the stackPointer is incremented,
//      and the compiler + CPU need to know that this order matters.
//  (2) When popping an frame from the stack and then preparing the
//      ProfilingStackFrame data for the next frame that is about to be pushed,
//      the decrement of the stackPointer in pop() needs to happen *before* the
//      ProfilingStackFrame for the new frame is being popuplated, and the
//      compiler + CPU need to know that this order matters.
//
// We can express the relevance of these orderings in multiple ways.
// Option A is to make stackPointer an atomic with SequentiallyConsistent
// memory ordering. This would ensure that no writes in thread A would be
// reordered across any writes to stackPointer, which satisfies requirements
// (1) and (2) at the same time. Option A is the simplest.
// Option B is to use ReleaseAcquire memory ordering both for writes to
// stackPointer *and* for writes to ProfilingStackFrame fields. Release-stores
// ensure that all writes that happened *before this write in program order* are
// not reordered to happen after this write. ReleaseAcquire ordering places no
// requirements on the ordering of writes that happen *after* this write in
// program order.
// Using release-stores for writes to stackPointer expresses requirement (1),
// and using release-stores for writes to the ProfilingStackFrame fields
// expresses requirement (2).
//
// Option B is more complicated than option A, but has much better performance
// on x86/64: In a microbenchmark run on a Macbook Pro from 2017, switching
// from option A to option B reduced the overhead of pushing+popping a
// ProfilingStackFrame by 10 nanoseconds.
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
// to functions push/pop a stack frame to/from the specified stack.
//
// For more detailed information, see vm/GeckoProfiler.h.
//
class ProfilingStackFrame {
  // A ProfilingStackFrame represents either a label frame or a JS frame.

  // WARNING WARNING WARNING
  //
  // All the fields below are Atomic<...,ReleaseAcquire>. This is needed so
  // that writes to these fields are release-writes, which ensures that
  // earlier writes in this thread don't get reordered after the writes to
  // these fields. In particular, the decrement of the stack pointer in
  // ProfilingStack::pop() is a write that *must* happen before the values in
  // this ProfilingStackFrame are changed. Otherwise, the sampler thread might
  // see an inconsistent state where the stack pointer still points to a
  // ProfilingStackFrame which has already been popped off the stack and whose
  // fields have now been partially repopulated with new values.
  // See the "Concurrency considerations" paragraph at the top of this file
  // for more details.

  // Descriptive label for this stack frame. Must be a static string! Can be
  // an empty string, but not a null pointer.
  mozilla::Atomic<const char*, mozilla::ReleaseAcquire> label_;

  // An additional descriptive string of this frame which is combined with
  // |label_| in profiler output. Need not be (and usually isn't) static. Can
  // be null.
  mozilla::Atomic<const char*, mozilla::ReleaseAcquire> dynamicString_;

  // Stack pointer for non-JS stack frames, the script pointer otherwise.
  mozilla::Atomic<void*, mozilla::ReleaseAcquire> spOrScript;

  // ID of the JS Realm for JS stack frames.
  // Must not be used on non-JS frames; it'll contain either the default 0,
  // or a leftover value from a previous JS stack frame that was using this
  // ProfilingStackFrame object.
  mozilla::Atomic<uint64_t, mozilla::ReleaseAcquire> realmID_;

  // The bytecode offset for JS stack frames.
  // Must not be used on non-JS frames; it'll contain either the default 0,
  // or a leftover value from a previous JS stack frame that was using this
  // ProfilingStackFrame object.
  mozilla::Atomic<int32_t, mozilla::ReleaseAcquire> pcOffsetIfJS_;

  // Bits 0...8 hold the Flags. Bits 9...31 hold the category pair.
  mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> flagsAndCategoryPair_;

  static int32_t pcToOffset(JSScript* aScript, jsbytecode* aPc);

 public:
  ProfilingStackFrame() = default;
  ProfilingStackFrame& operator=(const ProfilingStackFrame& other) {
    label_ = other.label();
    dynamicString_ = other.dynamicString();
    void* spScript = other.spOrScript;
    spOrScript = spScript;
    int32_t offsetIfJS = other.pcOffsetIfJS_;
    pcOffsetIfJS_ = offsetIfJS;
    uint64_t realmID = other.realmID_;
    realmID_ = realmID;
    uint32_t flagsAndCategory = other.flagsAndCategoryPair_;
    flagsAndCategoryPair_ = flagsAndCategory;
    return *this;
  }

  // Reserve up to 16 bits for flags, and 16 for category pair.
  enum class Flags : uint32_t {
    // The first three flags describe the kind of the frame and are
    // mutually exclusive. (We still give them individual bits for
    // simplicity.)

    // A regular label frame. These usually come from AutoProfilerLabel.
    IS_LABEL_FRAME = 1 << 0,

    // A special frame indicating the start of a run of JS profiling stack
    // frames. IS_SP_MARKER_FRAME frames are ignored, except for the sp
    // field. These frames are needed to get correct ordering between JS
    // and LABEL frames because JS frames don't carry sp information.
    // SP is short for "stack pointer".
    IS_SP_MARKER_FRAME = 1 << 1,

    // A JS frame.
    IS_JS_FRAME = 1 << 2,

    // An interpreter JS frame that has OSR-ed into baseline. IS_JS_FRAME
    // frames can have this flag set and unset during their lifetime.
    // JS_OSR frames are ignored.
    JS_OSR = 1 << 3,

    // The next three are mutually exclusive.
    // By default, for profiling stack frames that have both a label and a
    // dynamic string, the two strings are combined into one string of the
    // form "<label> <dynamicString>" during JSON serialization. The
    // following flags can be used to change this preset.
    STRING_TEMPLATE_METHOD = 1 << 4,  // "<label>.<dynamicString>"
    STRING_TEMPLATE_GETTER = 1 << 5,  // "get <label>.<dynamicString>"
    STRING_TEMPLATE_SETTER = 1 << 6,  // "set <label>.<dynamicString>"

    // If set, causes this stack frame to be marked as "relevantForJS" in
    // the profile JSON, which will make it show up in the "JS only" call
    // tree view.
    RELEVANT_FOR_JS = 1 << 7,

    // If set, causes the label on this ProfilingStackFrame to be ignored
    // and to be replaced by the subcategory's label.
    LABEL_DETERMINED_BY_CATEGORY_PAIR = 1 << 8,

    // Frame dynamic string does not contain user data.
    NONSENSITIVE = 1 << 9,

    // A JS Baseline Interpreter frame.
    IS_BLINTERP_FRAME = 1 << 10,

    FLAGS_BITCOUNT = 16,
    FLAGS_MASK = (1 << FLAGS_BITCOUNT) - 1
  };

  static_assert(
      uint32_t(JS::ProfilingCategoryPair::LAST) <=
          (UINT32_MAX >> uint32_t(Flags::FLAGS_BITCOUNT)),
      "Too many category pairs to fit into u32 with together with the "
      "reserved bits for the flags");

  bool isLabelFrame() const {
    return uint32_t(flagsAndCategoryPair_) & uint32_t(Flags::IS_LABEL_FRAME);
  }

  bool isNonsensitive() const {
    return uint32_t(flagsAndCategoryPair_) & uint32_t(Flags::NONSENSITIVE);
  }

  bool isSpMarkerFrame() const {
    return uint32_t(flagsAndCategoryPair_) &
           uint32_t(Flags::IS_SP_MARKER_FRAME);
  }

  bool isJsFrame() const {
    return uint32_t(flagsAndCategoryPair_) & uint32_t(Flags::IS_JS_FRAME);
  }

  bool isJsBlinterpFrame() const {
    return uint32_t(flagsAndCategoryPair_) & uint32_t(Flags::IS_BLINTERP_FRAME);
  }

  bool isOSRFrame() const {
    return uint32_t(flagsAndCategoryPair_) & uint32_t(Flags::JS_OSR);
  }

  void setIsOSRFrame(bool isOSR) {
    if (isOSR) {
      flagsAndCategoryPair_ =
          uint32_t(flagsAndCategoryPair_) | uint32_t(Flags::JS_OSR);
    } else {
      flagsAndCategoryPair_ =
          uint32_t(flagsAndCategoryPair_) & ~uint32_t(Flags::JS_OSR);
    }
  }

  void setLabelCategory(JS::ProfilingCategoryPair aCategoryPair) {
    MOZ_ASSERT(isLabelFrame());
    flagsAndCategoryPair_ =
        (uint32_t(aCategoryPair) << uint32_t(Flags::FLAGS_BITCOUNT)) | flags();
  }

  const char* label() const {
    uint32_t flagsAndCategoryPair = flagsAndCategoryPair_;
    if (flagsAndCategoryPair &
        uint32_t(Flags::LABEL_DETERMINED_BY_CATEGORY_PAIR)) {
      auto categoryPair = JS::ProfilingCategoryPair(
          flagsAndCategoryPair >> uint32_t(Flags::FLAGS_BITCOUNT));
      return JS::GetProfilingCategoryPairInfo(categoryPair).mLabel;
    }
    return label_;
  }

  const char* dynamicString() const { return dynamicString_; }

  void initLabelFrame(const char* aLabel, const char* aDynamicString, void* sp,
                      JS::ProfilingCategoryPair aCategoryPair,
                      uint32_t aFlags) {
    label_ = aLabel;
    dynamicString_ = aDynamicString;
    spOrScript = sp;
    // pcOffsetIfJS_ is not set and must not be used on label frames.
    flagsAndCategoryPair_ =
        uint32_t(Flags::IS_LABEL_FRAME) |
        (uint32_t(aCategoryPair) << uint32_t(Flags::FLAGS_BITCOUNT)) | aFlags;
    MOZ_ASSERT(isLabelFrame());
  }

  void initSpMarkerFrame(void* sp) {
    label_ = "";
    dynamicString_ = nullptr;
    spOrScript = sp;
    // pcOffsetIfJS_ is not set and must not be used on sp marker frames.
    flagsAndCategoryPair_ = uint32_t(Flags::IS_SP_MARKER_FRAME) |
                            (uint32_t(JS::ProfilingCategoryPair::OTHER)
                             << uint32_t(Flags::FLAGS_BITCOUNT));
    MOZ_ASSERT(isSpMarkerFrame());
  }

  template <JS::ProfilingCategoryPair Category, uint32_t ExtraFlags = 0>
  void initJsFrame(const char* aLabel, const char* aDynamicString,
                   JSScript* aScript, jsbytecode* aPc, uint64_t aRealmID) {
    label_ = aLabel;
    dynamicString_ = aDynamicString;
    spOrScript = aScript;
    pcOffsetIfJS_ = pcToOffset(aScript, aPc);
    realmID_ = aRealmID;
    flagsAndCategoryPair_ =
        (uint32_t(Category) << uint32_t(Flags::FLAGS_BITCOUNT)) |
        uint32_t(Flags::IS_JS_FRAME) | ExtraFlags;
    MOZ_ASSERT(isJsFrame());
  }

  uint32_t flags() const {
    return uint32_t(flagsAndCategoryPair_) & uint32_t(Flags::FLAGS_MASK);
  }

  JS::ProfilingCategoryPair categoryPair() const {
    return JS::ProfilingCategoryPair(flagsAndCategoryPair_ >>
                                     uint32_t(Flags::FLAGS_BITCOUNT));
  }

  uint64_t realmID() const { return realmID_; }

  void* stackAddress() const {
    MOZ_ASSERT(!isJsFrame());
    return spOrScript;
  }

  JS_PUBLIC_API JSScript* script() const;

  JS_PUBLIC_API JSFunction* function() const;

  // Note that the pointer returned might be invalid.
  JSScript* rawScript() const {
    MOZ_ASSERT(isJsFrame());
    void* script = spOrScript;
    return static_cast<JSScript*>(script);
  }

  // We can't know the layout of JSScript, so look in vm/GeckoProfiler.cpp.
  JS_PUBLIC_API jsbytecode* pc() const;
  void setPC(jsbytecode* pc);

  void trace(JSTracer* trc);

  // The offset of a pc into a script's code can actually be 0, so to
  // signify a nullptr pc, use a -1 index. This is checked against in
  // pc() and setPC() to set/get the right pc.
  static const int32_t NullPCOffset = -1;
};

JS_PUBLIC_API void SetContextProfilingStack(JSContext* cx,
                                            ProfilingStack* profilingStack);

// GetContextProfilingStack also exists, but it's defined in RootingAPI.h.

JS_PUBLIC_API void EnableContextProfilingStack(JSContext* cx, bool enabled);

JS_PUBLIC_API void RegisterContextProfilingEventMarker(JSContext* cx,
                                                       void (*fn)(const char*,
                                                                  const char*));

}  // namespace js

namespace JS {

typedef ProfilingStack* (*RegisterThreadCallback)(const char* threadName,
                                                  void* stackBase);

typedef void (*UnregisterThreadCallback)();

// regiserThread and unregisterThread callbacks are functions which are called
// by other threads without any locking mechanism.
JS_PUBLIC_API void SetProfilingThreadCallbacks(
    RegisterThreadCallback registerThread,
    UnregisterThreadCallback unregisterThread);

}  // namespace JS

// Each thread has its own ProfilingStack. That thread modifies the
// ProfilingStack, pushing and popping elements as necessary.
//
// The ProfilingStack is also read periodically by the profiler's sampler
// thread. This happens only when the thread that owns the ProfilingStack is
// suspended. So there are no genuine parallel accesses.
//
// However, it is possible for pushing/popping to be interrupted by a periodic
// sample. Because of this, we need pushing/popping to be effectively atomic.
//
// - When pushing a new frame, we increment the stack pointer -- making the new
//   frame visible to the sampler thread -- only after the new frame has been
//   fully written. The stack pointer is Atomic<uint32_t,ReleaseAcquire>, so
//   the increment is a release-store, which ensures that this store is not
//   reordered before the writes of the frame.
//
// - When popping an old frame, the only operation is the decrementing of the
//   stack pointer, which is obviously atomic.
//
class JS_PUBLIC_API ProfilingStack final {
 public:
  ProfilingStack() = default;

  ~ProfilingStack();

  void pushLabelFrame(const char* label, const char* dynamicString, void* sp,
                      JS::ProfilingCategoryPair categoryPair,
                      uint32_t flags = 0) {
    // This thread is the only one that ever changes the value of
    // stackPointer.
    // Store the value of the atomic in a non-atomic local variable so that
    // the compiler won't generate two separate loads from the atomic for
    // the size check and the frames[] array indexing operation.
    uint32_t stackPointerVal = stackPointer;

    if (MOZ_UNLIKELY(stackPointerVal >= capacity)) {
      ensureCapacitySlow();
    }
    frames[stackPointerVal].initLabelFrame(label, dynamicString, sp,
                                           categoryPair, flags);

    // This must happen at the end! The compiler will not reorder this
    // update because stackPointer is Atomic<..., ReleaseAcquire>, so any
    // the writes above will not be reordered below the stackPointer store.
    // Do the read and the write as two separate statements, in order to
    // make it clear that we don't need an atomic increment, which would be
    // more expensive on x86 than the separate operations done here.
    // However, don't use stackPointerVal here; instead, allow the compiler
    // to turn this store into a non-atomic increment instruction which
    // takes up less code size.
    stackPointer = stackPointer + 1;
  }

  void pushSpMarkerFrame(void* sp) {
    uint32_t oldStackPointer = stackPointer;

    if (MOZ_UNLIKELY(oldStackPointer >= capacity)) {
      ensureCapacitySlow();
    }
    frames[oldStackPointer].initSpMarkerFrame(sp);

    // This must happen at the end, see the comment in pushLabelFrame.
    stackPointer = oldStackPointer + 1;
  }

  void pushJsFrame(const char* label, const char* dynamicString,
                   JSScript* script, jsbytecode* pc, uint64_t aRealmID) {
    // This thread is the only one that ever changes the value of
    // stackPointer. Only load the atomic once.
    uint32_t oldStackPointer = stackPointer;

    if (MOZ_UNLIKELY(oldStackPointer >= capacity)) {
      ensureCapacitySlow();
    }
    frames[oldStackPointer]
        .initJsFrame<JS::ProfilingCategoryPair::JS_Interpreter>(
            label, dynamicString, script, pc, aRealmID);

    // This must happen at the end, see the comment in pushLabelFrame.
    stackPointer = stackPointer + 1;
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

  uint32_t stackSize() const { return stackPointer; }
  uint32_t stackCapacity() const { return capacity; }

 private:
  // Out of line path for expanding the buffer, since otherwise this would get
  // inlined in every DOM WebIDL call.
  MOZ_COLD void ensureCapacitySlow();

  // No copying.
  ProfilingStack(const ProfilingStack&) = delete;
  void operator=(const ProfilingStack&) = delete;

  // No moving either.
  ProfilingStack(ProfilingStack&&) = delete;
  void operator=(ProfilingStack&&) = delete;

  uint32_t capacity = 0;

 public:
  // The pointer to the stack frames, this is read from the profiler thread and
  // written from the current thread.
  //
  // This is effectively a unique pointer.
  mozilla::Atomic<js::ProfilingStackFrame*, mozilla::SequentiallyConsistent>
      frames{nullptr};

  // This may exceed the capacity, so instead use the stackSize() method to
  // determine the number of valid frames in stackFrames. When this is less
  // than stackCapacity(), it refers to the first free stackframe past the top
  // of the in-use stack (i.e. frames[stackPointer - 1] is the top stack
  // frame).
  //
  // WARNING WARNING WARNING
  //
  // This is an atomic variable that uses ReleaseAcquire memory ordering.
  // See the "Concurrency considerations" paragraph at the top of this file
  // for more details.
  mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> stackPointer{0};
};

namespace js {

class AutoGeckoProfilerEntry;
class GeckoProfilerEntryMarker;
class GeckoProfilerBaselineOSRMarker;

class GeckoProfilerThread {
  friend class AutoGeckoProfilerEntry;
  friend class GeckoProfilerEntryMarker;
  friend class GeckoProfilerBaselineOSRMarker;

  ProfilingStack* profilingStack_;

  // Same as profilingStack_ if the profiler is currently active, otherwise
  // null.
  ProfilingStack* profilingStackIfEnabled_;

 public:
  GeckoProfilerThread();

  uint32_t stackPointer() {
    MOZ_ASSERT(infraInstalled());
    return profilingStack_->stackPointer;
  }
  ProfilingStackFrame* stack() { return profilingStack_->frames; }
  ProfilingStack* getProfilingStack() { return profilingStack_; }
  ProfilingStack* getProfilingStackIfEnabled() {
    return profilingStackIfEnabled_;
  }

  /*
   * True if the profiler infrastructure is setup.  Should be true in builds
   * that include profiler support except during early startup or late
   * shutdown.  Unrelated to the presence of the Gecko Profiler addon.
   */
  bool infraInstalled() { return profilingStack_ != nullptr; }

  void setProfilingStack(ProfilingStack* profilingStack, bool enabled);
  void enable(bool enable) {
    profilingStackIfEnabled_ = enable ? profilingStack_ : nullptr;
  }
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
  bool enter(JSContext* cx, JSScript* script);
  void exit(JSContext* cx, JSScript* script);
  inline void updatePC(JSContext* cx, JSScript* script, jsbytecode* pc);
};

}  // namespace js

#endif /* js_ProfilingStack_h */
