/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GeckoProfiler_h
#define vm_GeckoProfiler_h

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"

#include <stddef.h>
#include <stdint.h>

#include "jspubtd.h"

#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/ProfilingCategory.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "threading/ProtectedData.h"

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
 * dynamically. Each stack frame has type ProfilingStackFrame.
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
 * information may be updated on the ProfilingStackFrame structure.
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
 * this, each ProfilingStackFrame has a 'pc' field to tell where its execution
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
 * this in place, only the top frame of the stack can ever have nullptr as its
 * pc. Additionally with this invariant, it is possible to maintain mappings of
 * JIT code to pc which can be accessed safely because they will only be
 * accessed from a signal handler when the JIT code is executing.
 */

class JS_PUBLIC_API ProfilingStack;

namespace js {

class BaseScript;
class GeckoProfilerThread;

// The `ProfileStringMap` weakly holds its `BaseScript*` keys and owns its
// string values. Entries are removed when the `BaseScript` is finalized; see
// `GeckoProfiler::onScriptFinalized`.
using ProfileStringMap = HashMap<BaseScript*, JS::UniqueChars,
                                 DefaultHasher<BaseScript*>, SystemAllocPolicy>;

class GeckoProfilerRuntime {
  JSRuntime* rt;
  MainThreadData<ProfileStringMap> strings_;
  bool slowAssertions;
  uint32_t enabled_;
  void (*eventMarker_)(const char*, const char*);

 public:
  explicit GeckoProfilerRuntime(JSRuntime* rt);

  /* management of whether instrumentation is on or off */
  bool enabled() { return enabled_; }
  void enable(bool enabled);
  void enableSlowAssertions(bool enabled) { slowAssertions = enabled; }
  bool slowAssertionsEnabled() { return slowAssertions; }

  void setEventMarker(void (*fn)(const char*, const char*));

  static JS::UniqueChars allocProfileString(JSContext* cx, BaseScript* script);
  const char* profileString(JSContext* cx, BaseScript* script);

  void onScriptFinalized(BaseScript* script);

  void markEvent(const char* event, const char* details);

  ProfileStringMap& strings() { return strings_.ref(); }

  /* meant to be used for testing, not recommended to call in normal code */
  size_t stringsCount();
  void stringsReset();

  uint32_t* addressOfEnabled() { return &enabled_; }

  void fixupStringsMapAfterMovingGC();
#ifdef JSGC_HASH_TABLE_CHECKS
  void checkStringsMapAfterMovingGC();
#endif
};

inline size_t GeckoProfilerRuntime::stringsCount() { return strings().count(); }

inline void GeckoProfilerRuntime::stringsReset() { strings().clear(); }

/*
 * This class is used in RunScript() to push the marker onto the sampling stack
 * that we're about to enter JS function calls. This is the only time in which a
 * valid stack pointer is pushed to the sampling stack.
 */
class MOZ_RAII GeckoProfilerEntryMarker {
 public:
  explicit MOZ_ALWAYS_INLINE GeckoProfilerEntryMarker(JSContext* cx,
                                                      JSScript* script);
  MOZ_ALWAYS_INLINE ~GeckoProfilerEntryMarker();

 private:
  GeckoProfilerThread* profiler_;
#ifdef DEBUG
  uint32_t spBefore_;
#endif
};

/*
 * RAII class to automatically add Gecko Profiler profiling stack frames.
 * It retrieves the ProfilingStack from the JSContext and does nothing if the
 * profiler is inactive.
 *
 * NB: The `label` string must be statically allocated.
 */
class MOZ_RAII AutoGeckoProfilerEntry {
 public:
  explicit MOZ_ALWAYS_INLINE AutoGeckoProfilerEntry(
      JSContext* cx, const char* label, const char* dynamicString,
      JS::ProfilingCategoryPair categoryPair = JS::ProfilingCategoryPair::JS,
      uint32_t flags = 0);
  explicit MOZ_ALWAYS_INLINE AutoGeckoProfilerEntry(
      JSContext* cx, const char* label,
      JS::ProfilingCategoryPair categoryPair = JS::ProfilingCategoryPair::JS,
      uint32_t flags = 0);
  MOZ_ALWAYS_INLINE ~AutoGeckoProfilerEntry();

 private:
  ProfilingStack* profilingStack_;
#ifdef DEBUG
  GeckoProfilerThread* profiler_;
  uint32_t spBefore_;
#endif
};

/*
 * Use this RAII class to add Gecko Profiler label frames for methods of the
 * JavaScript builtin API.
 * These frames will be exposed to JavaScript developers (ie they won't be
 * filtered out when using the "JavaScript" filtering option in the Firefox
 * Profiler UI).
 * Technical note: the label and dynamicString values will be joined with a dot
 * separator if dynamicString is present.
 */
class MOZ_RAII AutoJSMethodProfilerEntry : public AutoGeckoProfilerEntry {
 public:
  explicit MOZ_ALWAYS_INLINE AutoJSMethodProfilerEntry(
      JSContext* cx, const char* label, const char* dynamicString = nullptr);
};

/*
 * Use this RAII class to add Gecko Profiler label frames for constructors of
 * the JavaScript builtin API.
 * These frames will be exposed to JavaScript developers (ie they won't be
 * filtered out when using the "JavaScript" filtering option in the Firefox
 * Profiler UI).
 * Technical note: the word "constructor" will be appended to the label (with a
 * space separator).
 */
class MOZ_RAII AutoJSConstructorProfilerEntry : public AutoGeckoProfilerEntry {
 public:
  explicit MOZ_ALWAYS_INLINE AutoJSConstructorProfilerEntry(JSContext* cx,
                                                            const char* label);
};

/*
 * This class is used in the interpreter to bound regions where the baseline JIT
 * being entered via OSR.  It marks the current top profiling stack frame as
 * OSR-ed
 */
class MOZ_RAII GeckoProfilerBaselineOSRMarker {
 public:
  explicit GeckoProfilerBaselineOSRMarker(JSContext* cx, bool hasProfilerFrame);
  ~GeckoProfilerBaselineOSRMarker();

 private:
  GeckoProfilerThread* profiler;
  mozilla::DebugOnly<uint32_t> spBefore_;
};

} /* namespace js */

#endif /* vm_GeckoProfiler_h */
