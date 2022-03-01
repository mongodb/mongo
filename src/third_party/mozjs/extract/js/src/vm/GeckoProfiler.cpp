/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GeckoProfiler-inl.h"

#include "mozilla/Sprintf.h"

#include "jsnum.h"

#include "gc/GC.h"
#include "gc/PublicIterators.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineJIT.h"
#include "jit/JitcodeMap.h"
#include "jit/JitRuntime.h"
#include "jit/JSJitFrameIter.h"
#include "js/ProfilingStack.h"
#include "js/TraceLoggerAPI.h"
#include "util/StringBuffer.h"
#include "vm/FrameIter.h"  // js::OnlyJSJitFrameIter
#include "vm/JSScript.h"

#include "gc/Marking-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;

GeckoProfilerThread::GeckoProfilerThread()
    : profilingStack_(nullptr), profilingStackIfEnabled_(nullptr) {}

GeckoProfilerRuntime::GeckoProfilerRuntime(JSRuntime* rt)
    : rt(rt),
      strings_(),
      slowAssertions(false),
      enabled_(false),
      eventMarker_(nullptr) {
  MOZ_ASSERT(rt != nullptr);
}

void GeckoProfilerThread::setProfilingStack(ProfilingStack* profilingStack,
                                            bool enabled) {
  profilingStack_ = profilingStack;
  profilingStackIfEnabled_ = enabled ? profilingStack : nullptr;
}

void GeckoProfilerRuntime::setEventMarker(void (*fn)(const char*,
                                                     const char*)) {
  eventMarker_ = fn;
}

// Get a pointer to the top-most profiling frame, given the exit frame pointer.
static void* GetTopProfilingJitFrame(Activation* act) {
  if (!act || !act->isJit()) {
    return nullptr;
  }

  jit::JitActivation* jitActivation = act->asJit();

  // If there is no exit frame set, just return.
  if (!jitActivation->hasExitFP()) {
    return nullptr;
  }

  // Skip wasm frames that might be in the way.
  OnlyJSJitFrameIter iter(jitActivation);
  if (iter.done()) {
    return nullptr;
  }

  jit::JSJitProfilingFrameIterator jitIter(
      (jit::CommonFrameLayout*)iter.frame().fp());
  MOZ_ASSERT(!jitIter.done());
  return jitIter.fp();
}

void GeckoProfilerRuntime::enable(bool enabled) {
  JSContext* cx = rt->mainContextFromAnyThread();
  MOZ_ASSERT(cx->geckoProfiler().infraInstalled());

  if (enabled_ == enabled) {
    return;
  }

  /*
   * Ensure all future generated code will be instrumented, or that all
   * currently instrumented code is discarded
   */
  ReleaseAllJITCode(rt->defaultFreeOp());

  // This function is called when the Gecko profiler makes a new Sampler
  // (and thus, a new circular buffer). Set all current entries in the
  // JitcodeGlobalTable as expired and reset the buffer range start.
  if (rt->hasJitRuntime() && rt->jitRuntime()->hasJitcodeGlobalTable()) {
    rt->jitRuntime()->getJitcodeGlobalTable()->setAllEntriesAsExpired();
  }
  rt->setProfilerSampleBufferRangeStart(0);

  // Ensure that lastProfilingFrame is null for the main thread.
  if (cx->jitActivation) {
    cx->jitActivation->setLastProfilingFrame(nullptr);
    cx->jitActivation->setLastProfilingCallSite(nullptr);
  }

  // Reset the tracelogger, if toggled on
  JS::ResetTraceLogger();

  enabled_ = enabled;

  /* Toggle Gecko Profiler-related jumps on baseline jitcode.
   * The call to |ReleaseAllJITCode| above will release most baseline jitcode,
   * but not jitcode for scripts with active frames on the stack.  These scripts
   * need to have their profiler state toggled so they behave properly.
   */
  jit::ToggleBaselineProfiling(cx, enabled);

  // Update lastProfilingFrame to point to the top-most JS jit-frame currently
  // on stack.
  if (cx->jitActivation) {
    // Walk through all activations, and set their lastProfilingFrame
    // appropriately.
    if (enabled) {
      Activation* act = cx->activation();
      void* lastProfilingFrame = GetTopProfilingJitFrame(act);

      jit::JitActivation* jitActivation = cx->jitActivation;
      while (jitActivation) {
        jitActivation->setLastProfilingFrame(lastProfilingFrame);
        jitActivation->setLastProfilingCallSite(nullptr);

        jitActivation = jitActivation->prevJitActivation();
        lastProfilingFrame = GetTopProfilingJitFrame(jitActivation);
      }
    } else {
      jit::JitActivation* jitActivation = cx->jitActivation;
      while (jitActivation) {
        jitActivation->setLastProfilingFrame(nullptr);
        jitActivation->setLastProfilingCallSite(nullptr);
        jitActivation = jitActivation->prevJitActivation();
      }
    }
  }

  // WebAssembly code does not need to be released, but profiling string
  // labels have to be generated so that they are available during async
  // profiling stack iteration.
  for (RealmsIter r(rt); !r.done(); r.next()) {
    r->wasm.ensureProfilingLabels(enabled);
  }

#ifdef JS_STRUCTURED_SPEW
  // Enable the structured spewer if the environment variable is set.
  if (enabled) {
    cx->spewer().enableSpewing();
  } else {
    cx->spewer().disableSpewing();
  }
#endif
}

/* Lookup the string for the function/script, creating one if necessary */
const char* GeckoProfilerRuntime::profileString(JSContext* cx,
                                                BaseScript* script) {
  ProfileStringMap::AddPtr s = strings().lookupForAdd(script);

  if (!s) {
    UniqueChars str = allocProfileString(cx, script);
    if (!str) {
      return nullptr;
    }
    MOZ_ASSERT(script->hasBytecode());
    if (!strings().add(s, script, std::move(str))) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
  }

  return s->value().get();
}

void GeckoProfilerRuntime::onScriptFinalized(BaseScript* script) {
  /*
   * This function is called whenever a script is destroyed, regardless of
   * whether profiling has been turned on, so don't invoke a function on an
   * invalid hash set. Also, even if profiling was enabled but then turned
   * off, we still want to remove the string, so no check of enabled() is
   * done.
   */
  if (ProfileStringMap::Ptr entry = strings().lookup(script)) {
    strings().remove(entry);
  }
}

void GeckoProfilerRuntime::markEvent(const char* event, const char* details) {
  MOZ_ASSERT(enabled());
  if (eventMarker_) {
    JS::AutoSuppressGCAnalysis nogc;
    eventMarker_(event, details);
  }
}

bool GeckoProfilerThread::enter(JSContext* cx, JSScript* script) {
  const char* dynamicString =
      cx->runtime()->geckoProfiler().profileString(cx, script);
  if (dynamicString == nullptr) {
    return false;
  }

#ifdef DEBUG
  // In debug builds, assert the JS profiling stack frames already on the
  // stack have a non-null pc. Only look at the top frames to avoid quadratic
  // behavior.
  uint32_t sp = profilingStack_->stackPointer;
  if (sp > 0 && sp - 1 < profilingStack_->stackCapacity()) {
    size_t start = (sp > 4) ? sp - 4 : 0;
    for (size_t i = start; i < sp - 1; i++) {
      MOZ_ASSERT_IF(profilingStack_->frames[i].isJsFrame(),
                    profilingStack_->frames[i].pc());
    }
  }
#endif

  profilingStack_->pushJsFrame(
      "", dynamicString, script, script->code(),
      script->realm()->creationOptions().profilerRealmID());
  return true;
}

void GeckoProfilerThread::exit(JSContext* cx, JSScript* script) {
  profilingStack_->pop();

#ifdef DEBUG
  /* Sanity check to make sure push/pop balanced */
  uint32_t sp = profilingStack_->stackPointer;
  if (sp < profilingStack_->stackCapacity()) {
    JSRuntime* rt = script->runtimeFromMainThread();
    const char* dynamicString = rt->geckoProfiler().profileString(cx, script);
    /* Can't fail lookup because we should already be in the set */
    MOZ_ASSERT(dynamicString);

    // Bug 822041
    if (!profilingStack_->frames[sp].isJsFrame()) {
      fprintf(stderr, "--- ABOUT TO FAIL ASSERTION ---\n");
      fprintf(stderr, " frames=%p size=%u/%u\n", (void*)profilingStack_->frames,
              uint32_t(profilingStack_->stackPointer),
              profilingStack_->stackCapacity());
      for (int32_t i = sp; i >= 0; i--) {
        ProfilingStackFrame& frame = profilingStack_->frames[i];
        if (frame.isJsFrame()) {
          fprintf(stderr, "  [%d] JS %s\n", i, frame.dynamicString());
        } else {
          fprintf(stderr, "  [%d] Label %s\n", i, frame.dynamicString());
        }
      }
    }

    ProfilingStackFrame& frame = profilingStack_->frames[sp];
    MOZ_ASSERT(frame.isJsFrame());
    MOZ_ASSERT(frame.script() == script);
    MOZ_ASSERT(strcmp((const char*)frame.dynamicString(), dynamicString) == 0);
  }
#endif
}

/*
 * Serializes the script/function pair into a "descriptive string" which is
 * allowed to fail. This function cannot trigger a GC because it could finalize
 * some scripts, resize the hash table of profile strings, and invalidate the
 * AddPtr held while invoking allocProfileString.
 */
/* static */
UniqueChars GeckoProfilerRuntime::allocProfileString(JSContext* cx,
                                                     BaseScript* script) {
  // Note: this profiler string is regexp-matched by
  // devtools/client/profiler/cleopatra/js/parserWorker.js.

  // If the script has a function, try calculating its name.
  bool hasName = false;
  size_t nameLength = 0;
  UniqueChars nameStr;
  JSFunction* func = script->function();
  if (func && func->displayAtom()) {
    nameStr = StringToNewUTF8CharsZ(cx, *func->displayAtom());
    if (!nameStr) {
      return nullptr;
    }

    nameLength = strlen(nameStr.get());
    hasName = true;
  }

  // Calculate filename length. We cap this to a reasonable limit to avoid
  // performance impact of strlen/alloc/memcpy.
  constexpr size_t MaxFilenameLength = 200;
  const char* filenameStr = script->filename() ? script->filename() : "(null)";
  size_t filenameLength = js_strnlen(filenameStr, MaxFilenameLength);

  // Calculate line + column length.
  bool hasLineAndColumn = false;
  size_t lineAndColumnLength = 0;
  char lineAndColumnStr[30];
  if (hasName || script->isFunction() || script->isForEval()) {
    lineAndColumnLength = SprintfLiteral(lineAndColumnStr, "%u:%u",
                                         script->lineno(), script->column());
    hasLineAndColumn = true;
  }

  // Full profile string for scripts with functions is:
  //      FuncName (FileName:Lineno:Column)
  // Full profile string for scripts without functions is:
  //      FileName:Lineno:Column
  // Full profile string for scripts without functions and without lines is:
  //      FileName

  // Calculate full string length.
  size_t fullLength = 0;
  if (hasName) {
    MOZ_ASSERT(hasLineAndColumn);
    fullLength = nameLength + 2 + filenameLength + 1 + lineAndColumnLength + 1;
  } else if (hasLineAndColumn) {
    fullLength = filenameLength + 1 + lineAndColumnLength;
  } else {
    fullLength = filenameLength;
  }

  // Allocate string.
  UniqueChars str(cx->pod_malloc<char>(fullLength + 1));
  if (!str) {
    return nullptr;
  }

  size_t cur = 0;

  // Fill string with function name if needed.
  if (hasName) {
    memcpy(str.get() + cur, nameStr.get(), nameLength);
    cur += nameLength;
    str[cur++] = ' ';
    str[cur++] = '(';
  }

  // Fill string with filename chars.
  memcpy(str.get() + cur, filenameStr, filenameLength);
  cur += filenameLength;

  // Fill line + column chars.
  if (hasLineAndColumn) {
    str[cur++] = ':';
    memcpy(str.get() + cur, lineAndColumnStr, lineAndColumnLength);
    cur += lineAndColumnLength;
  }

  // Terminal ')' if necessary.
  if (hasName) {
    str[cur++] = ')';
  }

  MOZ_ASSERT(cur == fullLength);
  str[cur] = 0;

  return str;
}

void GeckoProfilerThread::trace(JSTracer* trc) {
  if (profilingStack_) {
    size_t size = profilingStack_->stackSize();
    for (size_t i = 0; i < size; i++) {
      profilingStack_->frames[i].trace(trc);
    }
  }
}

void GeckoProfilerRuntime::fixupStringsMapAfterMovingGC() {
  for (ProfileStringMap::Enum e(strings()); !e.empty(); e.popFront()) {
    BaseScript* script = e.front().key();
    if (IsForwarded(script)) {
      script = Forwarded(script);
      e.rekeyFront(script);
    }
  }
}

#ifdef JSGC_HASH_TABLE_CHECKS
void GeckoProfilerRuntime::checkStringsMapAfterMovingGC() {
  for (auto r = strings().all(); !r.empty(); r.popFront()) {
    BaseScript* script = r.front().key();
    CheckGCThingAfterMovingGC(script);
    auto ptr = strings().lookup(script);
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }
}
#endif

void ProfilingStackFrame::trace(JSTracer* trc) {
  if (isJsFrame()) {
    JSScript* s = rawScript();
    TraceNullableRoot(trc, &s, "ProfilingStackFrame script");
    spOrScript = s;
  }
}

GeckoProfilerBaselineOSRMarker::GeckoProfilerBaselineOSRMarker(
    JSContext* cx, bool hasProfilerFrame)
    : profiler(&cx->geckoProfiler()) {
  if (!hasProfilerFrame || !cx->runtime()->geckoProfiler().enabled()) {
    profiler = nullptr;
    return;
  }

  uint32_t sp = profiler->profilingStack_->stackPointer;
  if (sp >= profiler->profilingStack_->stackCapacity()) {
    profiler = nullptr;
    return;
  }

  spBefore_ = sp;
  if (sp == 0) {
    return;
  }

  ProfilingStackFrame& frame = profiler->profilingStack_->frames[sp - 1];
  MOZ_ASSERT(!frame.isOSRFrame());
  frame.setIsOSRFrame(true);
}

GeckoProfilerBaselineOSRMarker::~GeckoProfilerBaselineOSRMarker() {
  if (profiler == nullptr) {
    return;
  }

  uint32_t sp = profiler->stackPointer();
  MOZ_ASSERT(spBefore_ == sp);
  if (sp == 0) {
    return;
  }

  ProfilingStackFrame& frame = profiler->stack()[sp - 1];
  MOZ_ASSERT(frame.isOSRFrame());
  frame.setIsOSRFrame(false);
}

JS_PUBLIC_API JSScript* ProfilingStackFrame::script() const {
  MOZ_ASSERT(isJsFrame());
  auto script = reinterpret_cast<JSScript*>(spOrScript.operator void*());
  if (!script) {
    return nullptr;
  }

  // If profiling is supressed then we can't trust the script pointers to be
  // valid as they could be in the process of being moved by a compacting GC
  // (although it's still OK to get the runtime from them).
  JSContext* cx = script->runtimeFromAnyThread()->mainContextFromAnyThread();
  if (!cx->isProfilerSamplingEnabled()) {
    return nullptr;
  }

  MOZ_ASSERT(!IsForwarded(script));
  return script;
}

JS_PUBLIC_API JSFunction* ProfilingStackFrame::function() const {
  JSScript* script = this->script();
  return script ? script->function() : nullptr;
}

JS_PUBLIC_API jsbytecode* ProfilingStackFrame::pc() const {
  MOZ_ASSERT(isJsFrame());
  if (pcOffsetIfJS_ == NullPCOffset) {
    return nullptr;
  }

  JSScript* script = this->script();
  return script ? script->offsetToPC(pcOffsetIfJS_) : nullptr;
}

/* static */
int32_t ProfilingStackFrame::pcToOffset(JSScript* aScript, jsbytecode* aPc) {
  return aPc ? aScript->pcToOffset(aPc) : NullPCOffset;
}

void ProfilingStackFrame::setPC(jsbytecode* pc) {
  MOZ_ASSERT(isJsFrame());
  JSScript* script = this->script();
  MOZ_ASSERT(
      script);  // This should not be called while profiling is suppressed.
  pcOffsetIfJS_ = pcToOffset(script, pc);
}

JS_PUBLIC_API void js::SetContextProfilingStack(
    JSContext* cx, ProfilingStack* profilingStack) {
  cx->geckoProfiler().setProfilingStack(
      profilingStack, cx->runtime()->geckoProfiler().enabled());
}

JS_PUBLIC_API void js::EnableContextProfilingStack(JSContext* cx,
                                                   bool enabled) {
  cx->geckoProfiler().enable(enabled);
  cx->runtime()->geckoProfiler().enable(enabled);
}

JS_PUBLIC_API void js::RegisterContextProfilingEventMarker(
    JSContext* cx, void (*fn)(const char*, const char*)) {
  MOZ_ASSERT(cx->runtime()->geckoProfiler().enabled());
  cx->runtime()->geckoProfiler().setEventMarker(fn);
}

AutoSuppressProfilerSampling::AutoSuppressProfilerSampling(JSContext* cx)
    : cx_(cx), previouslyEnabled_(cx->isProfilerSamplingEnabled()) {
  if (previouslyEnabled_) {
    cx_->disableProfilerSampling();
  }
}

AutoSuppressProfilerSampling::~AutoSuppressProfilerSampling() {
  if (previouslyEnabled_) {
    cx_->enableProfilerSampling();
  }
}

namespace JS {

// clang-format off

// ProfilingSubcategory_X:
// One enum for each category X, listing that category's subcategories. This
// allows the sProfilingCategoryInfo macro construction below to look up a
// per-category index for a subcategory.
#define SUBCATEGORY_ENUMS_BEGIN_CATEGORY(name, labelAsString, color) \
  enum class ProfilingSubcategory_##name : uint32_t {
#define SUBCATEGORY_ENUMS_SUBCATEGORY(category, name, labelAsString) \
    name,
#define SUBCATEGORY_ENUMS_END_CATEGORY \
  };
MOZ_PROFILING_CATEGORY_LIST(SUBCATEGORY_ENUMS_BEGIN_CATEGORY,
                            SUBCATEGORY_ENUMS_SUBCATEGORY,
                            SUBCATEGORY_ENUMS_END_CATEGORY)
#undef SUBCATEGORY_ENUMS_BEGIN_CATEGORY
#undef SUBCATEGORY_ENUMS_SUBCATEGORY
#undef SUBCATEGORY_ENUMS_END_CATEGORY

// sProfilingCategoryPairInfo:
// A list of ProfilingCategoryPairInfos with the same order as
// ProfilingCategoryPair, which can be used to map a ProfilingCategoryPair to
// its information.
#define CATEGORY_INFO_BEGIN_CATEGORY(name, labelAsString, color)
#define CATEGORY_INFO_SUBCATEGORY(category, name, labelAsString) \
  {ProfilingCategory::category,                                  \
   uint32_t(ProfilingSubcategory_##category::name), labelAsString},
#define CATEGORY_INFO_END_CATEGORY
const ProfilingCategoryPairInfo sProfilingCategoryPairInfo[] = {
  MOZ_PROFILING_CATEGORY_LIST(CATEGORY_INFO_BEGIN_CATEGORY,
                              CATEGORY_INFO_SUBCATEGORY,
                              CATEGORY_INFO_END_CATEGORY)
};
#undef CATEGORY_INFO_BEGIN_CATEGORY
#undef CATEGORY_INFO_SUBCATEGORY
#undef CATEGORY_INFO_END_CATEGORY

// clang-format on

JS_PUBLIC_API const ProfilingCategoryPairInfo& GetProfilingCategoryPairInfo(
    ProfilingCategoryPair aCategoryPair) {
  static_assert(
      MOZ_ARRAY_LENGTH(sProfilingCategoryPairInfo) ==
          uint32_t(ProfilingCategoryPair::COUNT),
      "sProfilingCategoryPairInfo and ProfilingCategory need to have the "
      "same order and the same length");

  uint32_t categoryPairIndex = uint32_t(aCategoryPair);
  MOZ_RELEASE_ASSERT(categoryPairIndex <=
                     uint32_t(ProfilingCategoryPair::LAST));
  return sProfilingCategoryPairInfo[categoryPairIndex];
}

}  // namespace JS
