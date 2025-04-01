/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS execution context.
 */

#include "vm/JSContext-inl.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Utf8.h"  // mozilla::ConvertUtf16ToUtf8

#include <string.h>
#ifdef ANDROID
#  include <android/log.h>
#  include <fstream>
#  include <string>
#endif  // ANDROID
#ifdef XP_WIN
#  include <processthreadsapi.h>
#endif  // XP_WIN

#include "jsapi.h"  // JS_SetNativeStackQuota
#include "jsexn.h"
#include "jstypes.h"

#include "builtin/RegExp.h"  // js::RegExpSearcherLastLimitSentinel
#include "frontend/FrontendContext.h"
#include "gc/GC.h"
#include "irregexp/RegExpAPI.h"
#include "jit/Simulator.h"
#include "js/CallAndConstruct.h"  // JS::Call
#include "js/CharacterEncoding.h"
#include "js/ContextOptions.h"        // JS::ContextOptions
#include "js/ErrorInterceptor.h"      // JSErrorInterceptor
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::ReportOverRecursed
#include "js/MemoryCallbacks.h"
#include "js/Prefs.h"
#include "js/Printf.h"
#include "js/PropertyAndElement.h"  // JS_GetProperty
#include "js/Stack.h"  // JS::NativeStackSize, JS::NativeStackLimit, JS::NativeStackLimitMin
#include "util/DiagnosticAssertions.h"
#include "util/DifferentialTesting.h"
#include "util/DoubleToString.h"
#include "util/NativeStack.h"
#include "util/Text.h"
#include "util/WindowsWrapper.h"
#include "vm/BytecodeUtil.h"  // JSDVG_IGNORE_STACK
#include "vm/ErrorObject.h"
#include "vm/ErrorReporting.h"
#include "vm/FrameIter.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Realm.h"
#include "vm/StringType.h"  // StringToNewUTF8CharsZ
#include "vm/ToSource.h"    // js::ValueToSource

#include "vm/Compartment-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

#ifdef DEBUG
JSContext* js::MaybeGetJSContext() {
  if (!TlsContext.init()) {
    return nullptr;
  }
  return TlsContext.get();
}
#endif

bool js::AutoCycleDetector::init() {
  MOZ_ASSERT(cyclic);

  AutoCycleDetector::Vector& vector = cx->cycleDetectorVector();

  for (JSObject* obj2 : vector) {
    if (MOZ_UNLIKELY(obj == obj2)) {
      return true;
    }
  }

  if (!vector.append(obj)) {
    return false;
  }

  cyclic = false;
  return true;
}

js::AutoCycleDetector::~AutoCycleDetector() {
  if (MOZ_LIKELY(!cyclic)) {
    AutoCycleDetector::Vector& vec = cx->cycleDetectorVector();
    MOZ_ASSERT(vec.back() == obj);
    if (vec.length() > 1) {
      vec.popBack();
    } else {
      // Avoid holding on to unused heap allocations.
      vec.clearAndFree();
    }
  }
}

bool JSContext::init() {
  TlsContext.set(this);
  nativeStackBase_.emplace(GetNativeStackBase());

  if (!fx.initInstance()) {
    return false;
  }

#ifdef JS_SIMULATOR
  simulator_ = jit::Simulator::Create();
  if (!simulator_) {
    return false;
  }
#endif

  isolate = irregexp::CreateIsolate(this);
  if (!isolate) {
    return false;
  }

#ifdef DEBUG
  // Set the initialized_ last, so that ProtectedData checks will allow us to
  // initialize this context before it becomes the runtime's active context.
  initialized_ = true;
#endif

  return true;
}

static void InitDefaultStackQuota(JSContext* cx) {
  // Initialize stack quota to a reasonable default. Embedders can override this
  // by calling JS_SetNativeStackQuota.
  //
  // NOTE: Firefox overrides these values. For the main thread this happens in
  // XPCJSContext::Initialize.

#if defined(MOZ_ASAN) || (defined(DEBUG) && !defined(XP_WIN))
  static constexpr JS::NativeStackSize MaxStackSize =
      2 * 128 * sizeof(size_t) * 1024;
#else
  static constexpr JS::NativeStackSize MaxStackSize =
      128 * sizeof(size_t) * 1024;
#endif
  JS_SetNativeStackQuota(cx, MaxStackSize);
}

JSContext* js::NewContext(uint32_t maxBytes, JSRuntime* parentRuntime) {
  AutoNoteSingleThreadedRegion anstr;

  MOZ_RELEASE_ASSERT(!TlsContext.get());

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  js::oom::SetThreadType(!parentRuntime ? js::THREAD_TYPE_MAIN
                                        : js::THREAD_TYPE_WORKER);
#endif

  JSRuntime* runtime = js_new<JSRuntime>(parentRuntime);
  if (!runtime) {
    return nullptr;
  }

  JSContext* cx = js_new<JSContext>(runtime, JS::ContextOptions());
  if (!cx) {
    js_delete(runtime);
    return nullptr;
  }

  if (!cx->init()) {
    js_delete(cx);
    js_delete(runtime);
    return nullptr;
  }

  if (!runtime->init(cx, maxBytes)) {
    runtime->destroyRuntime();
    js_delete(cx);
    js_delete(runtime);
    return nullptr;
  }

  // Initialize stack quota last because simulators rely on the JSRuntime having
  // been initialized.
  InitDefaultStackQuota(cx);

  return cx;
}

void js::DestroyContext(JSContext* cx) {
  JS_AbortIfWrongThread(cx);

  MOZ_ASSERT(!cx->realm(), "Shouldn't destroy context with active realm");
  MOZ_ASSERT(!cx->activation(), "Shouldn't destroy context with activations");

  cx->checkNoGCRooters();

  // Cancel all off thread Ion compiles. Completed Ion compiles may try to
  // interrupt this context. See HelperThread::handleIonWorkload.
  CancelOffThreadIonCompile(cx->runtime());

  cx->jobQueue = nullptr;
  cx->internalJobQueue = nullptr;
  SetContextProfilingStack(cx, nullptr);

  JSRuntime* rt = cx->runtime();

  // Flush promise tasks executing in helper threads early, before any parts
  // of the JSRuntime that might be visible to helper threads are torn down.
  rt->offThreadPromiseState.ref().shutdown(cx);

  // Destroy the runtime along with its last context.
  js::AutoNoteSingleThreadedRegion nochecks;
  rt->destroyRuntime();
  js_delete_poison(cx);
  js_delete_poison(rt);
}

void JS::RootingContext::checkNoGCRooters() {
#ifdef DEBUG
  for (auto const& stackRootPtr : stackRoots_) {
    MOZ_ASSERT(stackRootPtr == nullptr);
  }
#endif
}

bool AutoResolving::alreadyStartedSlow() const {
  MOZ_ASSERT(link);
  AutoResolving* cursor = link;
  do {
    MOZ_ASSERT(this != cursor);
    if (object.get() == cursor->object && id.get() == cursor->id &&
        kind == cursor->kind) {
      return true;
    }
  } while (!!(cursor = cursor->link));
  return false;
}

static void MaybeReportOutOfMemoryForDifferentialTesting() {
  /*
   * OOMs are non-deterministic, especially across different execution modes
   * (e.g. interpreter vs JIT). When doing differential testing, print to stderr
   * so that the fuzzers can detect this.
   */
  if (js::SupportDifferentialTesting()) {
    fprintf(stderr, "ReportOutOfMemory called\n");
  }
}

/*
 * Since memory has been exhausted, avoid the normal error-handling path which
 * allocates an error object, report and callstack. Instead simply throw the
 * static atom "out of memory".
 *
 * Furthermore, callers of ReportOutOfMemory (viz., malloc) assume a GC does
 * not occur, so GC must be avoided or suppressed.
 */
void JSContext::onOutOfMemory() {
  runtime()->hadOutOfMemory = true;
  gc::AutoSuppressGC suppressGC(this);

  /* Report the oom. */
  if (JS::OutOfMemoryCallback oomCallback = runtime()->oomCallback) {
    oomCallback(this, runtime()->oomCallbackData);
  }

  // If we OOM early in process startup, this may be unavailable so just return
  // instead of crashing unexpectedly.
  if (MOZ_UNLIKELY(!runtime()->hasInitializedSelfHosting())) {
    return;
  }

  RootedValue oomMessage(this, StringValue(names().out_of_memory_));
  setPendingException(oomMessage, nullptr);
  MOZ_ASSERT(status == JS::ExceptionStatus::Throwing);
  status = JS::ExceptionStatus::OutOfMemory;

  reportResourceExhaustion();
}

JS_PUBLIC_API void js::ReportOutOfMemory(JSContext* cx) {
  MaybeReportOutOfMemoryForDifferentialTesting();

  cx->onOutOfMemory();
}

JS_PUBLIC_API void js::ReportLargeOutOfMemory(JSContext* cx) {
  js::ReportOutOfMemory(cx);
}

JS_PUBLIC_API void js::ReportOutOfMemory(FrontendContext* fc) {
  MaybeReportOutOfMemoryForDifferentialTesting();

  fc->onOutOfMemory();
}

static void MaybeReportOverRecursedForDifferentialTesting() {
  /*
   * We cannot make stack depth deterministic across different
   * implementations (e.g. JIT vs. interpreter will differ in
   * their maximum stack depth).
   * However, we can detect externally when we hit the maximum
   * stack depth which is useful for external testing programs
   * like fuzzers.
   */
  if (js::SupportDifferentialTesting()) {
    fprintf(stderr, "ReportOverRecursed called\n");
  }
}

void JSContext::onOverRecursed() {
  // Try to construct an over-recursed error and then update the exception
  // status to `OverRecursed`. Creating the error can fail, so check there
  // is a reasonable looking exception pending before updating status.
  JS_ReportErrorNumberASCII(this, GetErrorMessage, nullptr,
                            JSMSG_OVER_RECURSED);
  if (isExceptionPending() && !isThrowingOutOfMemory()) {
    MOZ_ASSERT(unwrappedException().isObject());
    MOZ_ASSERT(status == JS::ExceptionStatus::Throwing);
    status = JS::ExceptionStatus::OverRecursed;
  }

  reportResourceExhaustion();
}

JS_PUBLIC_API void js::ReportOverRecursed(JSContext* maybecx) {
  MaybeReportOverRecursedForDifferentialTesting();

  if (!maybecx) {
    return;
  }

  maybecx->onOverRecursed();
}

JS_PUBLIC_API void js::ReportOverRecursed(FrontendContext* fc) {
  MaybeReportOverRecursedForDifferentialTesting();

  fc->onOverRecursed();
}

void js::ReportOversizedAllocation(JSContext* cx, const unsigned errorNumber) {
  // The JIT may optimize away allocations if it determines that they aren't
  // used. This can affect whether we throw an exception when the size of an
  // allocation exceeds implementation-defined limits (eg JSString::MAX_LENGTH).
  // These errors aren't interesting for the purposes of differential fuzzing.
  // We print a message so that fuzzers can detect this case. To simplify
  // tooling updates, we use the same message as ReportOutOfMemory.
  if (js::SupportDifferentialTesting()) {
    fprintf(stderr, "ReportOutOfMemory called\n");
  }

  gc::AutoSuppressGC suppressGC(cx);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errorNumber);

  cx->reportResourceExhaustion();
}

void js::ReportAllocationOverflow(JSContext* cx) {
  if (js::SupportDifferentialTesting()) {
    fprintf(stderr, "ReportAllocationOverflow called\n");
  }

  if (!cx) {
    return;
  }

  cx->reportAllocationOverflow();
}

void js::ReportAllocationOverflow(FrontendContext* fc) {
  fc->onAllocationOverflow();
}

/* |callee| requires a usage string provided by JS_DefineFunctionsWithHelp. */
void js::ReportUsageErrorASCII(JSContext* cx, HandleObject callee,
                               const char* msg) {
  RootedValue usage(cx);
  if (!JS_GetProperty(cx, callee, "usage", &usage)) {
    return;
  }

  if (!usage.isString()) {
    JS_ReportErrorASCII(cx, "%s", msg);
  } else {
    RootedString usageStr(cx, usage.toString());
    UniqueChars str = JS_EncodeStringToUTF8(cx, usageStr);
    if (!str) {
      return;
    }
    JS_ReportErrorUTF8(cx, "%s. Usage: %s", msg, str.get());
  }
}

enum class PrintErrorKind { Error, Warning, Note };

static void PrintErrorLine(FILE* file, const char* prefix,
                           JSErrorReport* report) {
  if (const char16_t* linebuf = report->linebuf()) {
    UniqueChars line;
    size_t n;
    {
      size_t linebufLen = report->linebufLength();

      // This function is only used for shell command-line sorts of stuff where
      // performance doesn't really matter, so just encode into max-sized
      // memory.
      mozilla::CheckedInt<size_t> utf8Len(linebufLen);
      utf8Len *= 3;
      if (utf8Len.isValid()) {
        line = UniqueChars(js_pod_malloc<char>(utf8Len.value()));
        if (line) {
          n = mozilla::ConvertUtf16toUtf8({linebuf, linebufLen},
                                          {line.get(), utf8Len.value()});
        }
      }
    }

    const char* utf8buf;
    if (line) {
      utf8buf = line.get();
    } else {
      static const char unavailableStr[] = "<context unavailable>";
      utf8buf = unavailableStr;
      n = js_strlen(unavailableStr);
    }

    fputs(":\n", file);
    if (prefix) {
      fputs(prefix, file);
    }

    for (size_t i = 0; i < n; i++) {
      fputc(utf8buf[i], file);
    }

    // linebuf/utf8buf usually ends with a newline. If not, add one here.
    if (n == 0 || utf8buf[n - 1] != '\n') {
      fputc('\n', file);
    }

    if (prefix) {
      fputs(prefix, file);
    }

    n = report->tokenOffset();
    for (size_t i = 0, j = 0; i < n; i++) {
      if (utf8buf[i] == '\t') {
        for (size_t k = (j + 8) & ~7; j < k; j++) {
          fputc('.', file);
        }
        continue;
      }
      fputc('.', file);
      j++;
    }
    fputc('^', file);
  }
}

static void PrintErrorLine(FILE* file, const char* prefix,
                           JSErrorNotes::Note* note) {}

template <typename T>
static void PrintSingleError(FILE* file, JS::ConstUTF8CharsZ toStringResult,
                             T* report, PrintErrorKind kind) {
  UniqueChars prefix;
  if (report->filename) {
    prefix = JS_smprintf("%s:", report->filename.c_str());
  }

  if (report->lineno) {
    prefix = JS_smprintf("%s%u:%u ", prefix ? prefix.get() : "", report->lineno,
                         report->column.oneOriginValue());
  }

  if (kind != PrintErrorKind::Error) {
    const char* kindPrefix = nullptr;
    switch (kind) {
      case PrintErrorKind::Error:
        MOZ_CRASH("unreachable");
      case PrintErrorKind::Warning:
        kindPrefix = "warning";
        break;
      case PrintErrorKind::Note:
        kindPrefix = "note";
        break;
    }

    prefix = JS_smprintf("%s%s: ", prefix ? prefix.get() : "", kindPrefix);
  }

  const char* message =
      toStringResult ? toStringResult.c_str() : report->message().c_str();

  /* embedded newlines -- argh! */
  const char* ctmp;
  while ((ctmp = strchr(message, '\n')) != 0) {
    ctmp++;
    if (prefix) {
      fputs(prefix.get(), file);
    }
    (void)fwrite(message, 1, ctmp - message, file);
    message = ctmp;
  }

  /* If there were no filename or lineno, the prefix might be empty */
  if (prefix) {
    fputs(prefix.get(), file);
  }
  fputs(message, file);

  PrintErrorLine(file, prefix.get(), report);
  fputc('\n', file);

  fflush(file);
}

static void PrintErrorImpl(FILE* file, JS::ConstUTF8CharsZ toStringResult,
                           JSErrorReport* report, bool reportWarnings) {
  MOZ_ASSERT(report);

  /* Conditionally ignore reported warnings. */
  if (report->isWarning() && !reportWarnings) {
    return;
  }

  PrintErrorKind kind = PrintErrorKind::Error;
  if (report->isWarning()) {
    kind = PrintErrorKind::Warning;
  }
  PrintSingleError(file, toStringResult, report, kind);

  if (report->notes) {
    for (auto&& note : *report->notes) {
      PrintSingleError(file, JS::ConstUTF8CharsZ(), note.get(),
                       PrintErrorKind::Note);
    }
  }
}

JS_PUBLIC_API void JS::PrintError(FILE* file, JSErrorReport* report,
                                  bool reportWarnings) {
  PrintErrorImpl(file, JS::ConstUTF8CharsZ(), report, reportWarnings);
}

JS_PUBLIC_API void JS::PrintError(FILE* file,
                                  const JS::ErrorReportBuilder& builder,
                                  bool reportWarnings) {
  PrintErrorImpl(file, builder.toStringResult(), builder.report(),
                 reportWarnings);
}

void js::ReportIsNotDefined(JSContext* cx, HandleId id) {
  if (UniqueChars printable =
          IdToPrintableUTF8(cx, id, IdToPrintableBehavior::IdIsIdentifier)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_NOT_DEFINED,
                             printable.get());
  }
}

void js::ReportIsNotDefined(JSContext* cx, Handle<PropertyName*> name) {
  RootedId id(cx, NameToId(name));
  ReportIsNotDefined(cx, id);
}

const char* NullOrUndefinedToCharZ(HandleValue v) {
  MOZ_ASSERT(v.isNullOrUndefined());
  return v.isNull() ? "null" : "undefined";
}

void js::ReportIsNullOrUndefinedForPropertyAccess(JSContext* cx, HandleValue v,
                                                  int vIndex) {
  MOZ_ASSERT(v.isNullOrUndefined());

  if (vIndex == JSDVG_IGNORE_STACK) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CONVERT_TO, NullOrUndefinedToCharZ(v),
                              "object");
    return;
  }

  UniqueChars bytes = DecompileValueGenerator(cx, vIndex, v, nullptr);
  if (!bytes) {
    return;
  }

  if (strcmp(bytes.get(), "undefined") == 0 ||
      strcmp(bytes.get(), "null") == 0) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_NO_PROPERTIES,
                             bytes.get());
  } else {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_UNEXPECTED_TYPE, bytes.get(),
                             NullOrUndefinedToCharZ(v));
  }
}

void js::ReportIsNullOrUndefinedForPropertyAccess(JSContext* cx, HandleValue v,
                                                  int vIndex, HandleId key) {
  MOZ_ASSERT(v.isNullOrUndefined());

  if (!JS::Prefs::property_error_message_fix()) {
    ReportIsNullOrUndefinedForPropertyAccess(cx, v, vIndex);
    return;
  }

  RootedValue idVal(cx, IdToValue(key));
  RootedString idStr(cx, ValueToSource(cx, idVal));
  if (!idStr) {
    return;
  }

  UniqueChars keyStr = StringToNewUTF8CharsZ(cx, *idStr);
  if (!keyStr) {
    return;
  }

  if (vIndex == JSDVG_IGNORE_STACK) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_PROPERTY_FAIL,
                             keyStr.get(), NullOrUndefinedToCharZ(v));
    return;
  }

  UniqueChars bytes = DecompileValueGenerator(cx, vIndex, v, nullptr);
  if (!bytes) {
    return;
  }

  if (strcmp(bytes.get(), "undefined") == 0 ||
      strcmp(bytes.get(), "null") == 0) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_PROPERTY_FAIL,
                             keyStr.get(), bytes.get());
    return;
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_PROPERTY_FAIL_EXPR, keyStr.get(), bytes.get(),
                           NullOrUndefinedToCharZ(v));
}

bool js::ReportValueError(JSContext* cx, const unsigned errorNumber,
                          int spindex, HandleValue v, HandleString fallback,
                          const char* arg1, const char* arg2) {
  MOZ_ASSERT(js_ErrorFormatString[errorNumber].argCount >= 1);
  MOZ_ASSERT(js_ErrorFormatString[errorNumber].argCount <= 3);
  UniqueChars bytes = DecompileValueGenerator(cx, spindex, v, fallback);
  if (!bytes) {
    return false;
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber,
                           bytes.get(), arg1, arg2);
  return false;
}

JSObject* js::CreateErrorNotesArray(JSContext* cx, JSErrorReport* report) {
  Rooted<ArrayObject*> notesArray(cx, NewDenseEmptyArray(cx));
  if (!notesArray) {
    return nullptr;
  }

  if (!report->notes) {
    return notesArray;
  }

  for (auto&& note : *report->notes) {
    Rooted<PlainObject*> noteObj(cx, NewPlainObject(cx));
    if (!noteObj) {
      return nullptr;
    }

    RootedString messageStr(cx, note->newMessageString(cx));
    if (!messageStr) {
      return nullptr;
    }
    RootedValue messageVal(cx, StringValue(messageStr));
    if (!DefineDataProperty(cx, noteObj, cx->names().message, messageVal)) {
      return nullptr;
    }

    RootedValue filenameVal(cx);
    if (const char* filename = note->filename.c_str()) {
      JS::UTF8Chars utf8chars(filename, strlen(filename));
      Rooted<JSString*> filenameStr(cx, NewStringCopyUTF8N(cx, utf8chars));
      if (!filenameStr) {
        return nullptr;
      }
      filenameVal = StringValue(filenameStr);
    }
    if (!DefineDataProperty(cx, noteObj, cx->names().fileName, filenameVal)) {
      return nullptr;
    }

    RootedValue linenoVal(cx, Int32Value(note->lineno));
    if (!DefineDataProperty(cx, noteObj, cx->names().lineNumber, linenoVal)) {
      return nullptr;
    }
    RootedValue columnVal(cx, Int32Value(note->column.oneOriginValue()));
    if (!DefineDataProperty(cx, noteObj, cx->names().columnNumber, columnVal)) {
      return nullptr;
    }

    if (!NewbornArrayPush(cx, notesArray, ObjectValue(*noteObj))) {
      return nullptr;
    }
  }

  return notesArray;
}

void JSContext::recoverFromOutOfMemory() {
  if (isExceptionPending()) {
    MOZ_ASSERT(isThrowingOutOfMemory());
    clearPendingException();
  }
}

void JSContext::reportAllocationOverflow() {
  gc::AutoSuppressGC suppressGC(this);
  JS_ReportErrorNumberASCII(this, GetErrorMessage, nullptr,
                            JSMSG_ALLOC_OVERFLOW);
}

JS::StackKind JSContext::stackKindForCurrentPrincipal() {
  return runningWithTrustedPrincipals() ? JS::StackForTrustedScript
                                        : JS::StackForUntrustedScript;
}

JS::NativeStackLimit JSContext::stackLimitForCurrentPrincipal() {
  return stackLimit(stackKindForCurrentPrincipal());
}

JS_PUBLIC_API bool js::UseInternalJobQueues(JSContext* cx) {
  // Internal job queue handling must be set up very early. Self-hosting
  // initialization is as good a marker for that as any.
  MOZ_RELEASE_ASSERT(
      !cx->runtime()->hasInitializedSelfHosting(),
      "js::UseInternalJobQueues must be called early during runtime startup.");
  MOZ_ASSERT(!cx->jobQueue);
  auto queue = MakeUnique<InternalJobQueue>(cx);
  if (!queue) {
    return false;
  }

  cx->internalJobQueue = std::move(queue);
  cx->jobQueue = cx->internalJobQueue.ref().get();

  cx->runtime()->offThreadPromiseState.ref().initInternalDispatchQueue();
  MOZ_ASSERT(cx->runtime()->offThreadPromiseState.ref().initialized());

  return true;
}

#ifdef DEBUG
JSObject* InternalJobQueue::copyJobs(JSContext* cx) {
  Rooted<ArrayObject*> jobs(cx, NewDenseEmptyArray(cx));
  if (!jobs) {
    return nullptr;
  }

  for (const JSObject* unwrappedJob : queue.get()) {
    RootedObject job(cx, const_cast<JSObject*>(unwrappedJob));
    if (!cx->compartment()->wrap(cx, &job)) {
      return nullptr;
    }

    if (!NewbornArrayPush(cx, jobs, ObjectValue(*job))) {
      return nullptr;
    }
  }

  return jobs;
}

JS_PUBLIC_API JSObject* js::GetJobsInInternalJobQueue(JSContext* cx) {
  MOZ_ASSERT(cx->internalJobQueue.ref());
  return cx->internalJobQueue->copyJobs(cx);
}
#endif

JS_PUBLIC_API bool js::EnqueueJob(JSContext* cx, JS::HandleObject job) {
  MOZ_ASSERT(cx->jobQueue);
  return cx->jobQueue->enqueuePromiseJob(cx, nullptr, job, nullptr, nullptr);
}

JS_PUBLIC_API void js::StopDrainingJobQueue(JSContext* cx) {
  MOZ_ASSERT(cx->internalJobQueue.ref());
  cx->internalJobQueue->interrupt();
}

JS_PUBLIC_API void js::RestartDrainingJobQueue(JSContext* cx) {
  MOZ_ASSERT(cx->internalJobQueue.ref());
  cx->internalJobQueue->uninterrupt();
}

JS_PUBLIC_API void js::RunJobs(JSContext* cx) {
  MOZ_ASSERT(cx->jobQueue);
  MOZ_ASSERT(cx->isEvaluatingModule == 0);
  cx->jobQueue->runJobs(cx);
  JS::ClearKeptObjects(cx);
}

JSObject* InternalJobQueue::getIncumbentGlobal(JSContext* cx) {
  if (!cx->compartment()) {
    return nullptr;
  }
  return cx->global();
}

bool InternalJobQueue::enqueuePromiseJob(JSContext* cx,
                                         JS::HandleObject promise,
                                         JS::HandleObject job,
                                         JS::HandleObject allocationSite,
                                         JS::HandleObject incumbentGlobal) {
  MOZ_ASSERT(job);
  if (!queue.pushBack(job)) {
    ReportOutOfMemory(cx);
    return false;
  }

  JS::JobQueueMayNotBeEmpty(cx);
  return true;
}

void InternalJobQueue::runJobs(JSContext* cx) {
  if (draining_ || interrupted_) {
    return;
  }

  while (true) {
    cx->runtime()->offThreadPromiseState.ref().internalDrain(cx);

    // It doesn't make sense for job queue draining to be reentrant. At the
    // same time we don't want to assert against it, because that'd make
    // drainJobQueue unsafe for fuzzers. We do want fuzzers to test this,
    // so we simply ignore nested calls of drainJobQueue.
    draining_ = true;

    RootedObject job(cx);
    JS::HandleValueArray args(JS::HandleValueArray::empty());
    RootedValue rval(cx);

    // Execute jobs in a loop until we've reached the end of the queue.
    while (!queue.empty()) {
      // A previous job might have set this flag. E.g., the js shell
      // sets it if the `quit` builtin function is called.
      if (interrupted_) {
        break;
      }

      job = queue.front();
      queue.popFront();

      // If the next job is the last job in the job queue, allow
      // skipping the standard job queuing behavior.
      if (queue.empty()) {
        JS::JobQueueIsEmpty(cx);
      }

      AutoRealm ar(cx, &job->as<JSFunction>());
      {
        if (!JS::Call(cx, UndefinedHandleValue, job, args, &rval)) {
          // Nothing we can do about uncatchable exceptions.
          if (!cx->isExceptionPending()) {
            continue;
          }

          // Always clear the exception, because
          // PrepareScriptEnvironmentAndInvoke will assert that we don't have
          // one.
          RootedValue exn(cx);
          bool success = cx->getPendingException(&exn);
          cx->clearPendingException();
          if (success) {
            js::ReportExceptionClosure reportExn(exn);
            PrepareScriptEnvironmentAndInvoke(cx, cx->global(), reportExn);
          }
        }
      }
    }

    draining_ = false;

    if (interrupted_) {
      break;
    }

    queue.clear();

    // It's possible a job added a new off-thread promise task.
    if (!cx->runtime()->offThreadPromiseState.ref().internalHasPending()) {
      break;
    }
  }
}

bool InternalJobQueue::empty() const { return queue.empty(); }

JSObject* InternalJobQueue::maybeFront() const {
  if (queue.empty()) {
    return nullptr;
  }

  return queue.get().front();
}

class js::InternalJobQueue::SavedQueue : public JobQueue::SavedJobQueue {
 public:
  SavedQueue(JSContext* cx, Queue&& saved, bool draining)
      : cx(cx), saved(cx, std::move(saved)), draining_(draining) {
    MOZ_ASSERT(cx->internalJobQueue.ref());
  }

  ~SavedQueue() {
    MOZ_ASSERT(cx->internalJobQueue.ref());
    cx->internalJobQueue->queue = std::move(saved.get());
    cx->internalJobQueue->draining_ = draining_;
  }

 private:
  JSContext* cx;
  PersistentRooted<Queue> saved;
  bool draining_;
};

js::UniquePtr<JS::JobQueue::SavedJobQueue> InternalJobQueue::saveJobQueue(
    JSContext* cx) {
  auto saved =
      js::MakeUnique<SavedQueue>(cx, std::move(queue.get()), draining_);
  if (!saved) {
    // When MakeUnique's allocation fails, the SavedQueue constructor is never
    // called, so this->queue is still initialized. (The move doesn't occur
    // until the constructor gets called.)
    ReportOutOfMemory(cx);
    return nullptr;
  }

  queue = Queue(SystemAllocPolicy());
  draining_ = false;
  return saved;
}

mozilla::GenericErrorResult<OOM> JSContext::alreadyReportedOOM() {
  MOZ_ASSERT(isThrowingOutOfMemory());
  return mozilla::Err(JS::OOM());
}

mozilla::GenericErrorResult<JS::Error> JSContext::alreadyReportedError() {
  return mozilla::Err(JS::Error());
}

JSContext::JSContext(JSRuntime* runtime, const JS::ContextOptions& options)
    : RootingContext(runtime ? &runtime->gc.nursery() : nullptr),
      runtime_(runtime),
      options_(this, options),
      measuringExecutionTime_(this, false),
      jitActivation(this, nullptr),
      isolate(this, nullptr),
      activation_(this, nullptr),
      profilingActivation_(nullptr),
      entryMonitor(this, nullptr),
      noExecuteDebuggerTop(this, nullptr),
#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI
      inUnsafeCallWithABI(this, false),
      hasAutoUnsafeCallWithABI(this, false),
#endif
#ifdef DEBUG
      liveArraySortDataInstances(this, 0),
#endif
#ifdef JS_SIMULATOR
      simulator_(this, nullptr),
#endif
      dtoaState(this, nullptr),
      suppressGC(this, 0),
#ifdef FUZZING_JS_FUZZILLI
      executionHash(1),
      executionHashInputs(0),
#endif
#ifdef DEBUG
      noNurseryAllocationCheck(this, 0),
      disableStrictProxyCheckingCount(this, 0),
#endif
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
      runningOOMTest(this, false),
#endif
      inUnsafeRegion(this, 0),
      generationalDisabled(this, 0),
      compactingDisabledCount(this, 0),
#ifdef DEBUG
      regExpSearcherLastLimit(this, RegExpSearcherLastLimitSentinel),
#else
      regExpSearcherLastLimit(this, 0),
#endif
      isEvaluatingModule(this, 0),
      frontendCollectionPool_(this),
      suppressProfilerSampling(false),
      tempLifoAlloc_(this, (size_t)TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
      debuggerMutations(this, 0),
      status(this, JS::ExceptionStatus::None),
      unwrappedException_(this),
      unwrappedExceptionStack_(this),
#ifdef DEBUG
      hadResourceExhaustion_(this, false),
#endif
      reportGranularity(this, JS_DEFAULT_JITREPORT_GRANULARITY),
      resolvingList(this, nullptr),
#ifdef DEBUG
      enteredPolicy(this, nullptr),
#endif
      generatingError(this, false),
      cycleDetectorVector_(this, this),
      data(nullptr),
      asyncStackForNewActivations_(this),
      asyncCauseForNewActivations(this, nullptr),
      asyncCallIsExplicit(this, false),
      interruptCallbacks_(this),
      interruptCallbackDisabled(this, false),
      interruptBits_(0),
      inlinedICScript_(this, nullptr),
      jitStackLimit(JS::NativeStackLimitMin),
      jitStackLimitNoInterrupt(this, JS::NativeStackLimitMin),
      jobQueue(this, nullptr),
      internalJobQueue(this),
      canSkipEnqueuingJobs(this, false),
      promiseRejectionTrackerCallback(this, nullptr),
      promiseRejectionTrackerCallbackData(this, nullptr),
      insideExclusiveDebuggerOnEval(this, nullptr) {
  MOZ_ASSERT(static_cast<JS::RootingContext*>(this) ==
             JS::RootingContext::get(this));
}

#ifdef ENABLE_WASM_JSPI
bool js::IsSuspendableStackActive(JSContext* cx) {
  return cx->wasm().suspendableStackLimit != JS::NativeStackLimitMin;
}

JS::NativeStackLimit js::GetSuspendableStackLimit(JSContext* cx) {
  return cx->wasm().suspendableStackLimit;
}
#endif

JSContext::~JSContext() {
#ifdef DEBUG
  // Clear the initialized_ first, so that ProtectedData checks will allow us to
  // destroy this context even if the runtime is already gone.
  initialized_ = false;
#endif

  /* Free the stuff hanging off of cx. */
  MOZ_ASSERT(!resolvingList);

  // Ensure we didn't leak memory for the ArraySortData vector.
  MOZ_ASSERT(liveArraySortDataInstances == 0);

  if (dtoaState) {
    DestroyDtoaState(dtoaState);
  }

  fx.destroyInstance();

#ifdef JS_SIMULATOR
  js::jit::Simulator::Destroy(simulator_);
#endif

  if (isolate) {
    irregexp::DestroyIsolate(isolate.ref());
  }

  TlsContext.set(nullptr);
}

void JSContext::setRuntime(JSRuntime* rt) {
  MOZ_ASSERT(!resolvingList);
  MOZ_ASSERT(!compartment());
  MOZ_ASSERT(!activation());
  MOZ_ASSERT(!unwrappedException_.ref().initialized());
  MOZ_ASSERT(!unwrappedExceptionStack_.ref().initialized());
  MOZ_ASSERT(!asyncStackForNewActivations_.ref().initialized());

  runtime_ = rt;
}

#if defined(NIGHTLY_BUILD)
static bool IsOutOfMemoryException(JSContext* cx, const Value& v) {
  return v == StringValue(cx->names().out_of_memory_);
}
#endif

void JSContext::setPendingException(HandleValue v, Handle<SavedFrame*> stack) {
#if defined(NIGHTLY_BUILD)
  do {
    // Do not intercept exceptions if we are already
    // in the exception interceptor. That would lead
    // to infinite recursion.
    if (this->runtime()->errorInterception.isExecuting) {
      break;
    }

    // Check whether we have an interceptor at all.
    if (!this->runtime()->errorInterception.interceptor) {
      break;
    }

    // Don't report OOM exceptions. The interceptor isn't interested in those
    // and they can confuse the interceptor because OOM can be thrown when we
    // are not in a realm (atom allocation, for example).
    if (IsOutOfMemoryException(this, v)) {
      break;
    }

    // Make sure that we do not call the interceptor from within
    // the interceptor.
    this->runtime()->errorInterception.isExecuting = true;

    // The interceptor must be infallible.
    const mozilla::DebugOnly<bool> wasExceptionPending =
        this->isExceptionPending();
    this->runtime()->errorInterception.interceptor->interceptError(this, v);
    MOZ_ASSERT(wasExceptionPending == this->isExceptionPending());

    this->runtime()->errorInterception.isExecuting = false;
  } while (false);
#endif  // defined(NIGHTLY_BUILD)

  // overRecursed_ is set after the fact by ReportOverRecursed.
  this->status = JS::ExceptionStatus::Throwing;
  this->unwrappedException() = v;
  this->unwrappedExceptionStack() = stack;
}

void JSContext::setPendingException(HandleValue value,
                                    ShouldCaptureStack captureStack) {
  Rooted<SavedFrame*> nstack(this);
  if (captureStack == ShouldCaptureStack::Always ||
      realm()->shouldCaptureStackForThrow()) {
    RootedObject stack(this);
    if (!CaptureStack(this, &stack)) {
      clearPendingException();
    }
    if (stack) {
      nstack = &stack->as<SavedFrame>();
    }
  }
  setPendingException(value, nstack);
}

bool JSContext::getPendingException(MutableHandleValue rval) {
  MOZ_ASSERT(isExceptionPending());

  RootedValue exception(this, unwrappedException());
  if (zone()->isAtomsZone()) {
    rval.set(exception);
    return true;
  }

  Rooted<SavedFrame*> stack(this, unwrappedExceptionStack());
  JS::ExceptionStatus prevStatus = status;
  clearPendingException();
  if (!compartment()->wrap(this, &exception)) {
    return false;
  }
  this->check(exception);
  setPendingException(exception, stack);
  status = prevStatus;

  rval.set(exception);
  return true;
}

bool JSContext::getPendingExceptionStack(MutableHandleValue rval) {
  MOZ_ASSERT(isExceptionPending());

  Rooted<SavedFrame*> exceptionStack(this, unwrappedExceptionStack());
  if (!exceptionStack) {
    rval.setNull();
    return true;
  }
  if (zone()->isAtomsZone()) {
    rval.setObject(*exceptionStack);
    return true;
  }

  RootedValue stack(this, ObjectValue(*exceptionStack));
  RootedValue exception(this, unwrappedException());
  JS::ExceptionStatus prevStatus = status;
  clearPendingException();
  if (!compartment()->wrap(this, &exception) ||
      !compartment()->wrap(this, &stack)) {
    return false;
  }
  this->check(stack);
  setPendingException(exception, exceptionStack);
  status = prevStatus;

  rval.set(stack);
  return true;
}

SavedFrame* JSContext::getPendingExceptionStack() {
  return unwrappedExceptionStack();
}

#ifdef DEBUG
const JS::Value& JSContext::getPendingExceptionUnwrapped() {
  MOZ_ASSERT(isExceptionPending());
  return unwrappedException();
}
#endif

bool JSContext::isClosingGenerator() {
  return isExceptionPending() &&
         unwrappedException().isMagic(JS_GENERATOR_CLOSING);
}

bool JSContext::isThrowingDebuggeeWouldRun() {
  return isExceptionPending() && unwrappedException().isObject() &&
         unwrappedException().toObject().is<ErrorObject>() &&
         unwrappedException().toObject().as<ErrorObject>().type() ==
             JSEXN_DEBUGGEEWOULDRUN;
}

bool JSContext::isRuntimeCodeGenEnabled(JS::RuntimeCode kind,
                                        HandleString code) {
  // Make sure that the CSP callback is installed and that it permits runtime
  // code generation.
  if (JSCSPEvalChecker allows =
          runtime()->securityCallbacks->contentSecurityPolicyAllows) {
    return allows(this, kind, code);
  }

  return true;
}

size_t JSContext::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  /*
   * There are other JSContext members that could be measured; the following
   * ones have been found by DMD to be worth measuring.  More stuff may be
   * added later.
   */
  return cycleDetectorVector().sizeOfExcludingThis(mallocSizeOf) +
         irregexp::IsolateSizeOfIncludingThis(isolate, mallocSizeOf);
}

size_t JSContext::sizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
}

#ifdef DEBUG
bool JSContext::inAtomsZone() const { return zone_->isAtomsZone(); }
#endif

void JSContext::trace(JSTracer* trc) {
  cycleDetectorVector().trace(trc);
  geckoProfiler().trace(trc);
  if (isolate) {
    irregexp::TraceIsolate(trc, isolate.ref());
  }
#ifdef ENABLE_WASM_JSPI
  wasm().promiseIntegration.trace(trc);
#endif
}

JS::NativeStackLimit JSContext::stackLimitForJitCode(JS::StackKind kind) {
#ifdef JS_SIMULATOR
  return simulator()->stackLimit();
#else
  return stackLimit(kind);
#endif
}

void JSContext::resetJitStackLimit() {
  // Note that, for now, we use the untrusted limit for ion. This is fine,
  // because it's the most conservative limit, and if we hit it, we'll bail
  // out of ion into the interpreter, which will do a proper recursion check.
#ifdef JS_SIMULATOR
  jitStackLimit = jit::Simulator::StackLimit();
#else
  jitStackLimit = nativeStackLimit[JS::StackForUntrustedScript];
#endif
  jitStackLimitNoInterrupt = jitStackLimit;
}

void JSContext::initJitStackLimit() { resetJitStackLimit(); }

JSScript* JSContext::currentScript(jsbytecode** ppc,
                                   AllowCrossRealm allowCrossRealm) {
  if (ppc) {
    *ppc = nullptr;
  }

  // Fast path: there are no JS frames on the stack if there's no activation.
  if (!activation()) {
    return nullptr;
  }

  FrameIter iter(this);
  if (iter.done() || !iter.hasScript()) {
    return nullptr;
  }

  JSScript* script = iter.script();
  if (allowCrossRealm == AllowCrossRealm::DontAllow &&
      script->realm() != realm()) {
    return nullptr;
  }

  if (ppc) {
    *ppc = iter.pc();
  }
  return script;
}

#ifdef JS_CRASH_DIAGNOSTICS
void ContextChecks::check(AbstractFramePtr frame, int argIndex) {
  if (frame) {
    check(frame.realm(), argIndex);
  }
}
#endif

void AutoEnterOOMUnsafeRegion::crash_impl(const char* reason) {
  char msgbuf[1024];
  js::NoteIntentionalCrash();
  SprintfLiteral(msgbuf, "[unhandlable oom] %s", reason);
#ifndef DEBUG
  // In non-DEBUG builds MOZ_CRASH normally doesn't print to stderr so we have
  // to do this explicitly (the jit-test allow-unhandlable-oom annotation and
  // fuzzers depend on it).
  fprintf(stderr, "Hit MOZ_CRASH(%s) at %s:%d\n", msgbuf, __FILE__, __LINE__);
#endif
  MOZ_CRASH_UNSAFE(msgbuf);
}

mozilla::Atomic<AutoEnterOOMUnsafeRegion::AnnotateOOMAllocationSizeCallback,
                mozilla::Relaxed>
    AutoEnterOOMUnsafeRegion::annotateOOMSizeCallback(nullptr);

void AutoEnterOOMUnsafeRegion::crash_impl(size_t size, const char* reason) {
  {
    JS::AutoSuppressGCAnalysis suppress;
    if (annotateOOMSizeCallback) {
      annotateOOMSizeCallback(size);
    }
  }
  crash_impl(reason);
}

void ExternalValueArray::trace(JSTracer* trc) {
  if (Value* vp = begin()) {
    TraceRootRange(trc, length(), vp, "js::ExternalValueArray");
  }
}

#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI

AutoUnsafeCallWithABI::AutoUnsafeCallWithABI(UnsafeABIStrictness strictness)
    : cx_(TlsContext.get()),
      nested_(cx_ ? cx_->hasAutoUnsafeCallWithABI : false),
      nogc(cx_) {
  if (!cx_) {
    // This is a helper thread doing Ion or Wasm compilation - nothing to do.
    return;
  }
  switch (strictness) {
    case UnsafeABIStrictness::NoExceptions:
      MOZ_ASSERT(!JS_IsExceptionPending(cx_));
      checkForPendingException_ = true;
      break;
    case UnsafeABIStrictness::AllowPendingExceptions:
      checkForPendingException_ = !JS_IsExceptionPending(cx_);
      break;
    case UnsafeABIStrictness::AllowThrownExceptions:
      checkForPendingException_ = false;
      break;
  }

  cx_->hasAutoUnsafeCallWithABI = true;
}

AutoUnsafeCallWithABI::~AutoUnsafeCallWithABI() {
  if (!cx_) {
    return;
  }
  MOZ_ASSERT(cx_->hasAutoUnsafeCallWithABI);
  if (!nested_) {
    cx_->hasAutoUnsafeCallWithABI = false;
    cx_->inUnsafeCallWithABI = false;
  }
  MOZ_ASSERT_IF(checkForPendingException_, !JS_IsExceptionPending(cx_));
}

#endif  // JS_CHECK_UNSAFE_CALL_WITH_ABI

#ifdef __wasi__
JS_PUBLIC_API void js::IncWasiRecursionDepth(JSContext* cx) {
  ++JS::RootingContext::get(cx)->wasiRecursionDepth;
}

JS_PUBLIC_API void js::DecWasiRecursionDepth(JSContext* cx) {
  MOZ_ASSERT(JS::RootingContext::get(cx)->wasiRecursionDepth > 0);
  --JS::RootingContext::get(cx)->wasiRecursionDepth;
}

JS_PUBLIC_API bool js::CheckWasiRecursionLimit(JSContext* cx) {
  // WASI has two limits:
  // 1) The stack pointer in linear memory that grows to zero. See
  //    --stack-first in js/src/shell/moz.build.
  // 2) The JS::RootingContext::wasiRecursionDepth that counts recursion depth.
  //    Here we should check both.
  if (JS::RootingContext::get(cx)->wasiRecursionDepth >=
      JS::RootingContext::wasiRecursionDepthLimit) {
    return false;
  }
  return true;
}
#endif  // __wasi__
