/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS execution context.
 */

#include "vm/JSContext-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Unused.h"

#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#ifdef ANDROID
# include <android/log.h>
# include <fstream>
# include <string>
#endif // ANDROID
#ifdef XP_WIN
# include <processthreadsapi.h>
#endif // XP_WIN

#include "jsexn.h"
#include "jspubtd.h"
#include "jstypes.h"

#include "builtin/String.h"
#include "gc/FreeOp.h"
#include "gc/Marking.h"
#include "jit/Ion.h"
#include "jit/PcScriptCache.h"
#include "js/CharacterEncoding.h"
#include "js/Printf.h"
#include "util/DoubleToString.h"
#include "util/NativeStack.h"
#include "util/Windows.h"
#include "vm/BytecodeUtil.h"
#include "vm/ErrorReporting.h"
#include "vm/HelperThreads.h"
#include "vm/Iteration.h"
#include "vm/JSAtom.h"
#include "vm/JSCompartment.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Shape.h"
#include "wasm/WasmSignalHandlers.h"

#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::DebugOnly;
using mozilla::PodArrayZero;

bool
js::AutoCycleDetector::init()
{
    MOZ_ASSERT(cyclic);

    AutoCycleDetector::Vector& vector = cx->cycleDetectorVector();

    for (JSObject* obj2 : vector) {
        if (MOZ_UNLIKELY(obj == obj2))
            return true;
    }

    if (!vector.append(obj))
        return false;

    cyclic = false;
    return true;
}

js::AutoCycleDetector::~AutoCycleDetector()
{
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

bool
JSContext::init(ContextKind kind)
{
    // Skip most of the initialization if this thread will not be running JS.
    if (kind == ContextKind::Cooperative) {
        // Get a platform-native handle for this thread, used by js::InterruptRunningJitCode.
#ifdef XP_WIN
        size_t openFlags = THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME |
                           THREAD_QUERY_INFORMATION;
        HANDLE self = OpenThread(openFlags, false, GetCurrentThreadId());
        if (!self)
        return false;
        static_assert(sizeof(HANDLE) <= sizeof(threadNative_), "need bigger field");
        threadNative_ = (size_t)self;
#else
        static_assert(sizeof(pthread_t) <= sizeof(threadNative_), "need bigger field");
        threadNative_ = (size_t)pthread_self();
#endif

        if (!regexpStack.ref().init())
            return false;

        if (!fx.initInstance())
            return false;

#ifdef JS_SIMULATOR
        simulator_ = js::jit::Simulator::Create(this);
        if (!simulator_)
            return false;
#endif

        if (!wasm::EnsureSignalHandlers(this))
            return false;
    }

    // Set the ContextKind last, so that ProtectedData checks will allow us to
    // initialize this context before it becomes the runtime's active context.
    kind_ = kind;

    return true;
}

JSContext*
js::NewContext(uint32_t maxBytes, uint32_t maxNurseryBytes, JSRuntime* parentRuntime)
{
    AutoNoteSingleThreadedRegion anstr;

    MOZ_RELEASE_ASSERT(!TlsContext.get());

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    js::oom::SetThreadType(!parentRuntime ? js::THREAD_TYPE_COOPERATING
                                          : js::THREAD_TYPE_WORKER);
#endif

    JSRuntime* runtime = js_new<JSRuntime>(parentRuntime);
    if (!runtime)
        return nullptr;

    JSContext* cx = js_new<JSContext>(runtime, JS::ContextOptions());
    if (!cx) {
        js_delete(runtime);
        return nullptr;
    }

    if (!runtime->init(cx, maxBytes, maxNurseryBytes)) {
        runtime->destroyRuntime();
        js_delete(cx);
        js_delete(runtime);
        return nullptr;
    }

    if (!cx->init(ContextKind::Cooperative)) {
        runtime->destroyRuntime();
        js_delete(cx);
        js_delete(runtime);
        return nullptr;
    }

    return cx;
}

JSContext*
js::NewCooperativeContext(JSContext* siblingContext)
{
    MOZ_RELEASE_ASSERT(!TlsContext.get());

    JSRuntime* runtime = siblingContext->runtime();

    JSContext* cx = js_new<JSContext>(runtime, JS::ContextOptions());
    if (!cx || !cx->init(ContextKind::Cooperative)) {
        js_delete(cx);
        return nullptr;
    }

    runtime->setNewbornActiveContext(cx);
    return cx;
}

void
js::YieldCooperativeContext(JSContext* cx)
{
    MOZ_ASSERT(cx == TlsContext.get());
    MOZ_ASSERT(cx->runtime()->activeContext() == cx);
    cx->runtime()->setActiveContext(nullptr);
}

void
js::ResumeCooperativeContext(JSContext* cx)
{
    MOZ_ASSERT(cx == TlsContext.get());
    MOZ_ASSERT(cx->runtime()->activeContext() == nullptr);
    cx->runtime()->setActiveContext(cx);
}

static void
FreeJobQueueHandling(JSContext* cx)
{
    if (!cx->jobQueue)
        return;

    cx->jobQueue->reset();
    FreeOp* fop = cx->defaultFreeOp();
    fop->delete_(cx->jobQueue.ref());
    cx->getIncumbentGlobalCallback = nullptr;
    cx->enqueuePromiseJobCallback = nullptr;
    cx->enqueuePromiseJobCallbackData = nullptr;
}

void
js::DestroyContext(JSContext* cx)
{
    JS_AbortIfWrongThread(cx);

    if (cx->outstandingRequests != 0)
        MOZ_CRASH("Attempted to destroy a context while it is in a request.");

    cx->checkNoGCRooters();

    // Cancel all off thread Ion compiles before destroying a cooperative
    // context. Completed Ion compiles may try to interrupt arbitrary
    // cooperative contexts which they have read off the owner context of a
    // zone group. See HelperThread::handleIonWorkload.
    CancelOffThreadIonCompile(cx->runtime());

    FreeJobQueueHandling(cx);

    if (cx->runtime()->cooperatingContexts().length() == 1) {
        // Flush promise tasks executing in helper threads early, before any parts
        // of the JSRuntime that might be visible to helper threads are torn down.
        cx->runtime()->offThreadPromiseState.ref().shutdown(cx);

        // Destroy the runtime along with its last context.
        cx->runtime()->destroyRuntime();
        js_delete(cx->runtime());
        js_delete_poison(cx);
    } else {
        DebugOnly<bool> found = false;
        for (size_t i = 0; i < cx->runtime()->cooperatingContexts().length(); i++) {
            CooperatingContext& target = cx->runtime()->cooperatingContexts()[i];
            if (cx == target.context()) {
                cx->runtime()->cooperatingContexts().erase(&target);
                found = true;
                break;
            }
        }
        MOZ_ASSERT(found);

        cx->runtime()->deleteActiveContext(cx);
    }
}

void
JS::RootingContext::checkNoGCRooters() {
#ifdef DEBUG
    for (auto const& stackRootPtr : stackRoots_)
        MOZ_ASSERT(stackRootPtr == nullptr);
#endif
}

bool
AutoResolving::alreadyStartedSlow() const
{
    MOZ_ASSERT(link);
    AutoResolving* cursor = link;
    do {
        MOZ_ASSERT(this != cursor);
        if (object.get() == cursor->object && id.get() == cursor->id && kind == cursor->kind)
            return true;
    } while (!!(cursor = cursor->link));
    return false;
}

static void
ReportError(JSContext* cx, JSErrorReport* reportp, JSErrorCallback callback,
            void* userRef)
{
    /*
     * Check the error report, and set a JavaScript-catchable exception
     * if the error is defined to have an associated exception.  If an
     * exception is thrown, then the JSREPORT_EXCEPTION flag will be set
     * on the error report, and exception-aware hosts should ignore it.
     */
    MOZ_ASSERT(reportp);
    if ((!callback || callback == GetErrorMessage) &&
        reportp->errorNumber == JSMSG_UNCAUGHT_EXCEPTION)
    {
        reportp->flags |= JSREPORT_EXCEPTION;
    }

    if (JSREPORT_IS_WARNING(reportp->flags)) {
        CallWarningReporter(cx, reportp);
        return;
    }

    ErrorToException(cx, reportp, callback, userRef);
}

/*
 * The given JSErrorReport object have been zeroed and must not outlive
 * cx->fp() (otherwise owned fields may become invalid).
 */
static void
PopulateReportBlame(JSContext* cx, JSErrorReport* report)
{
    JSCompartment* compartment = cx->compartment();
    if (!compartment)
        return;

    /*
     * Walk stack until we find a frame that is associated with a non-builtin
     * rather than a builtin frame and which we're allowed to know about.
     */
    NonBuiltinFrameIter iter(cx, compartment->principals());
    if (iter.done())
        return;

    report->filename = iter.filename();
    report->lineno = iter.computeLine(&report->column);
    // XXX: Make the column 1-based as in other browsers, instead of 0-based
    // which is how SpiderMonkey stores it internally. This will be
    // unnecessary once bug 1144340 is fixed.
    report->column++;
    report->isMuted = iter.mutedErrors();
}

/*
 * Since memory has been exhausted, avoid the normal error-handling path which
 * allocates an error object, report and callstack. If code is running, simply
 * throw the static atom "out of memory". If code is not running, call the
 * error reporter directly.
 *
 * Furthermore, callers of ReportOutOfMemory (viz., malloc) assume a GC does
 * not occur, so GC must be avoided or suppressed.
 */
JS_FRIEND_API(void)
js::ReportOutOfMemory(JSContext* cx)
{
#ifdef JS_MORE_DETERMINISTIC
    /*
     * OOMs are non-deterministic, especially across different execution modes
     * (e.g. interpreter vs JIT). In more-deterministic builds, print to stderr
     * so that the fuzzers can detect this.
     */
    fprintf(stderr, "ReportOutOfMemory called\n");
#endif

    if (cx->helperThread())
        return cx->addPendingOutOfMemory();

    cx->runtime()->hadOutOfMemory = true;
    AutoSuppressGC suppressGC(cx);

    /* Report the oom. */
    if (JS::OutOfMemoryCallback oomCallback = cx->runtime()->oomCallback)
        oomCallback(cx, cx->runtime()->oomCallbackData);

    cx->setPendingException(StringValue(cx->names().outOfMemory));
}

mozilla::GenericErrorResult<OOM&>
js::ReportOutOfMemoryResult(JSContext* cx)
{
    ReportOutOfMemory(cx);
    return cx->alreadyReportedOOM();
}

void
js::ReportOverRecursed(JSContext* maybecx, unsigned errorNumber)
{
#ifdef JS_MORE_DETERMINISTIC
    /*
     * We cannot make stack depth deterministic across different
     * implementations (e.g. JIT vs. interpreter will differ in
     * their maximum stack depth).
     * However, we can detect externally when we hit the maximum
     * stack depth which is useful for external testing programs
     * like fuzzers.
     */
    fprintf(stderr, "ReportOverRecursed called\n");
#endif
    if (maybecx) {
        if (!maybecx->helperThread()) {
            JS_ReportErrorNumberASCII(maybecx, GetErrorMessage, nullptr, errorNumber);
            maybecx->overRecursed_ = true;
        } else {
            maybecx->addPendingOverRecursed();
        }
    }
}

JS_FRIEND_API(void)
js::ReportOverRecursed(JSContext* maybecx)
{
    ReportOverRecursed(maybecx, JSMSG_OVER_RECURSED);
}

void
js::ReportAllocationOverflow(JSContext* cx)
{
    if (!cx)
        return;

    if (cx->helperThread())
        return;

    AutoSuppressGC suppressGC(cx);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_ALLOC_OVERFLOW);
}

/*
 * Given flags and the state of cx, decide whether we should report an
 * error, a warning, or just continue execution normally.  Return
 * true if we should continue normally, without reporting anything;
 * otherwise, adjust *flags as appropriate and return false.
 */
static bool
checkReportFlags(JSContext* cx, unsigned* flags)
{
    if (JSREPORT_IS_STRICT(*flags)) {
        /* Warning/error only when JSOPTION_STRICT is set. */
        if (!cx->compartment()->behaviors().extraWarnings(cx))
            return true;
    }

    /* Warnings become errors when JSOPTION_WERROR is set. */
    if (JSREPORT_IS_WARNING(*flags) && cx->options().werror())
        *flags &= ~JSREPORT_WARNING;

    return false;
}

bool
js::ReportErrorVA(JSContext* cx, unsigned flags, const char* format,
                  ErrorArgumentsType argumentsType, va_list ap)
{
    JSErrorReport report;

    if (checkReportFlags(cx, &flags))
        return true;

    UniqueChars message(JS_vsmprintf(format, ap));
    if (!message) {
        ReportOutOfMemory(cx);
        return false;
    }

    MOZ_ASSERT_IF(argumentsType == ArgumentsAreASCII, JS::StringIsASCII(message.get()));

    report.flags = flags;
    report.errorNumber = JSMSG_USER_DEFINED_ERROR;
    if (argumentsType == ArgumentsAreASCII || argumentsType == ArgumentsAreUTF8) {
        report.initOwnedMessage(message.release());
    } else {
        MOZ_ASSERT(argumentsType == ArgumentsAreLatin1);
        Latin1Chars latin1(message.get(), strlen(message.get()));
        UTF8CharsZ utf8(JS::CharsToNewUTF8CharsZ(cx, latin1));
        if (!utf8)
            return false;
        report.initOwnedMessage(reinterpret_cast<const char*>(utf8.get()));
    }
    PopulateReportBlame(cx, &report);

    bool warning = JSREPORT_IS_WARNING(report.flags);

    ReportError(cx, &report, nullptr, nullptr);
    return warning;
}

/* |callee| requires a usage string provided by JS_DefineFunctionsWithHelp. */
void
js::ReportUsageErrorASCII(JSContext* cx, HandleObject callee, const char* msg)
{
    RootedValue usage(cx);
    if (!JS_GetProperty(cx, callee, "usage", &usage))
        return;

    if (!usage.isString()) {
        JS_ReportErrorASCII(cx, "%s", msg);
    } else {
        RootedString usageStr(cx, usage.toString());
        JSAutoByteString str;
        if (!str.encodeUtf8(cx, usageStr))
            return;
        JS_ReportErrorUTF8(cx, "%s. Usage: %s", msg, str.ptr());
    }
}

enum class PrintErrorKind {
    Error,
    Warning,
    StrictWarning,
    Note
};

static void
PrintErrorLine(FILE* file, const char* prefix, JSErrorReport* report)
{
    if (const char16_t* linebuf = report->linebuf()) {
        size_t n = report->linebufLength();

        fputs(":\n", file);
        if (prefix)
            fputs(prefix, file);

        for (size_t i = 0; i < n; i++)
            fputc(static_cast<char>(linebuf[i]), file);

        // linebuf usually ends with a newline. If not, add one here.
        if (n == 0 || linebuf[n-1] != '\n')
            fputc('\n', file);

        if (prefix)
            fputs(prefix, file);

        n = report->tokenOffset();
        for (size_t i = 0, j = 0; i < n; i++) {
            if (linebuf[i] == '\t') {
                for (size_t k = (j + 8) & ~7; j < k; j++)
                    fputc('.', file);
                continue;
            }
            fputc('.', file);
            j++;
        }
        fputc('^', file);
    }
}

static void
PrintErrorLine(FILE* file, const char* prefix, JSErrorNotes::Note* note)
{
}

template <typename T>
static bool
PrintSingleError(JSContext* cx, FILE* file, JS::ConstUTF8CharsZ toStringResult,
                 T* report, PrintErrorKind kind)
{
    UniqueChars prefix;
    if (report->filename)
        prefix = JS_smprintf("%s:", report->filename);

    if (report->lineno) {
        prefix = JS_smprintf("%s%u:%u ", prefix ? prefix.get() : "", report->lineno,
                                        report->column);
    }

    if (kind != PrintErrorKind::Error) {
        const char* kindPrefix = nullptr;
        switch (kind) {
          case PrintErrorKind::Error:
            MOZ_CRASH("unreachable");
          case PrintErrorKind::Warning:
            kindPrefix = "warning";
            break;
          case PrintErrorKind::StrictWarning:
            kindPrefix = "strict warning";
            break;
          case PrintErrorKind::Note:
            kindPrefix = "note";
            break;
        }

        prefix = JS_smprintf("%s%s: ", prefix ? prefix.get() : "", kindPrefix);
    }

    const char* message = toStringResult ? toStringResult.c_str() : report->message().c_str();

    /* embedded newlines -- argh! */
    const char* ctmp;
    while ((ctmp = strchr(message, '\n')) != 0) {
        ctmp++;
        if (prefix)
            fputs(prefix.get(), file);
        mozilla::Unused << fwrite(message, 1, ctmp - message, file);
        message = ctmp;
    }

    /* If there were no filename or lineno, the prefix might be empty */
    if (prefix)
        fputs(prefix.get(), file);
    fputs(message, file);

    PrintErrorLine(file, prefix.get(), report);
    fputc('\n', file);

    fflush(file);
    return true;
}

bool
js::PrintError(JSContext* cx, FILE* file, JS::ConstUTF8CharsZ toStringResult,
               JSErrorReport* report, bool reportWarnings)
{
    MOZ_ASSERT(report);

    /* Conditionally ignore reported warnings. */
    if (JSREPORT_IS_WARNING(report->flags) && !reportWarnings)
        return false;

    PrintErrorKind kind = PrintErrorKind::Error;
    if (JSREPORT_IS_WARNING(report->flags)) {
        if (JSREPORT_IS_STRICT(report->flags))
            kind = PrintErrorKind::StrictWarning;
        else
            kind = PrintErrorKind::Warning;
    }
    PrintSingleError(cx, file, toStringResult, report, kind);

    if (report->notes) {
        for (auto&& note : *report->notes)
            PrintSingleError(cx, file, JS::ConstUTF8CharsZ(), note.get(), PrintErrorKind::Note);
    }

    return true;
}

class MOZ_RAII AutoMessageArgs
{
    size_t totalLength_;
    /* only {0} thru {9} supported */
    mozilla::Array<const char*, JS::MaxNumErrorArguments> args_;
    mozilla::Array<size_t, JS::MaxNumErrorArguments> lengths_;
    uint16_t count_;
    bool allocatedElements_ : 1;

  public:
    AutoMessageArgs()
      : totalLength_(0), count_(0), allocatedElements_(false)
    {
        PodArrayZero(args_);
    }

    ~AutoMessageArgs()
    {
        /* free the arguments only if we allocated them */
        if (allocatedElements_) {
            uint16_t i = 0;
            while (i < count_) {
                if (args_[i])
                    js_free((void*)args_[i]);
                i++;
            }
        }
    }

    const char* args(size_t i) const {
        MOZ_ASSERT(i < count_);
        return args_[i];
    }

    size_t totalLength() const {
        return totalLength_;
    }

    size_t lengths(size_t i) const {
        MOZ_ASSERT(i < count_);
        return lengths_[i];
    }

    uint16_t count() const {
        return count_;
    }

    /* Gather the arguments into an array, and accumulate their sizes. */
    bool init(JSContext* cx, const char16_t** argsArg, uint16_t countArg,
              ErrorArgumentsType typeArg, va_list ap) {
        MOZ_ASSERT(countArg > 0);

        count_ = countArg;

        for (uint16_t i = 0; i < count_; i++) {
            switch (typeArg) {
              case ArgumentsAreASCII:
              case ArgumentsAreUTF8: {
                MOZ_ASSERT(!argsArg);
                args_[i] = va_arg(ap, char*);
                MOZ_ASSERT_IF(typeArg == ArgumentsAreASCII, JS::StringIsASCII(args_[i]));
                lengths_[i] = strlen(args_[i]);
                break;
              }
              case ArgumentsAreLatin1: {
                MOZ_ASSERT(!argsArg);
                const Latin1Char* latin1 = va_arg(ap, Latin1Char*);
                size_t len = strlen(reinterpret_cast<const char*>(latin1));
                mozilla::Range<const Latin1Char> range(latin1, len);
                char* utf8 = JS::CharsToNewUTF8CharsZ(cx, range).c_str();
                if (!utf8)
                    return false;

                args_[i] = utf8;
                lengths_[i] = strlen(utf8);
                allocatedElements_ = true;
                break;
              }
              case ArgumentsAreUnicode: {
                const char16_t* uc = argsArg ? argsArg[i] : va_arg(ap, char16_t*);
                size_t len = js_strlen(uc);
                mozilla::Range<const char16_t> range(uc, len);
                char* utf8 = JS::CharsToNewUTF8CharsZ(cx, range).c_str();
                if (!utf8)
                    return false;

                args_[i] = utf8;
                lengths_[i] = strlen(utf8);
                allocatedElements_ = true;
                break;
              }
            }
            totalLength_ += lengths_[i];
        }
        return true;
    }
};

static void
SetExnType(JSErrorReport* reportp, int16_t exnType)
{
    reportp->exnType = exnType;
}

static void
SetExnType(JSErrorNotes::Note* notep, int16_t exnType)
{
    // Do nothing for JSErrorNotes::Note.
}

/*
 * The arguments from ap need to be packaged up into an array and stored
 * into the report struct.
 *
 * The format string addressed by the error number may contain operands
 * identified by the format {N}, where N is a decimal digit. Each of these
 * is to be replaced by the Nth argument from the va_list. The complete
 * message is placed into reportp->message_.
 *
 * Returns true if the expansion succeeds (can fail if out of memory).
 */
template <typename T>
bool
ExpandErrorArgumentsHelper(JSContext* cx, JSErrorCallback callback,
                           void* userRef, const unsigned errorNumber,
                           const char16_t** messageArgs,
                           ErrorArgumentsType argumentsType,
                           T* reportp, va_list ap)
{
    const JSErrorFormatString* efs;

    if (!callback)
        callback = GetErrorMessage;

    {
        AutoSuppressGC suppressGC(cx);
        efs = callback(userRef, errorNumber);
    }

    if (efs) {
        SetExnType(reportp, efs->exnType);

        MOZ_ASSERT_IF(argumentsType == ArgumentsAreASCII, JS::StringIsASCII(efs->format));

        uint16_t argCount = efs->argCount;
        MOZ_RELEASE_ASSERT(argCount <= JS::MaxNumErrorArguments);
        if (argCount > 0) {
            /*
             * Parse the error format, substituting the argument X
             * for {X} in the format.
             */
            if (efs->format) {
                const char* fmt;
                char* out;
#ifdef DEBUG
                int expandedArgs = 0;
#endif
                size_t expandedLength;
                size_t len = strlen(efs->format);

                AutoMessageArgs args;
                if (!args.init(cx, messageArgs, argCount, argumentsType, ap))
                    return false;

                expandedLength = len
                                 - (3 * args.count()) /* exclude the {n} */
                                 + args.totalLength();

                /*
                * Note - the above calculation assumes that each argument
                * is used once and only once in the expansion !!!
                */
                char* utf8 = out = cx->pod_malloc<char>(expandedLength + 1);
                if (!out)
                    return false;

                fmt = efs->format;
                while (*fmt) {
                    if (*fmt == '{') {
                        if (isdigit(fmt[1])) {
                            int d = JS7_UNDEC(fmt[1]);
                            MOZ_RELEASE_ASSERT(d < args.count());
                            strncpy(out, args.args(d), args.lengths(d));
                            out += args.lengths(d);
                            fmt += 3;
#ifdef DEBUG
                            expandedArgs++;
#endif
                            continue;
                        }
                    }
                    *out++ = *fmt++;
                }
                MOZ_ASSERT(expandedArgs == args.count());
                *out = 0;

                reportp->initOwnedMessage(utf8);
            }
        } else {
            /* Non-null messageArgs should have at least one non-null arg. */
            MOZ_ASSERT(!messageArgs);
            /*
             * Zero arguments: the format string (if it exists) is the
             * entire message.
             */
            if (efs->format)
                reportp->initBorrowedMessage(efs->format);
        }
    }
    if (!reportp->message()) {
        /* where's the right place for this ??? */
        const char* defaultErrorMessage
            = "No error message available for error number %d";
        size_t nbytes = strlen(defaultErrorMessage) + 16;
        char* message = cx->pod_malloc<char>(nbytes);
        if (!message)
            return false;
        snprintf(message, nbytes, defaultErrorMessage, errorNumber);
        reportp->initOwnedMessage(message);
    }
    return true;
}

bool
js::ExpandErrorArgumentsVA(JSContext* cx, JSErrorCallback callback,
                           void* userRef, const unsigned errorNumber,
                           const char16_t** messageArgs,
                           ErrorArgumentsType argumentsType,
                           JSErrorReport* reportp, va_list ap)
{
    return ExpandErrorArgumentsHelper(cx, callback, userRef, errorNumber,
                                      messageArgs, argumentsType, reportp, ap);
}

bool
js::ExpandErrorArgumentsVA(JSContext* cx, JSErrorCallback callback,
                           void* userRef, const unsigned errorNumber,
                           const char16_t** messageArgs,
                           ErrorArgumentsType argumentsType,
                           JSErrorNotes::Note* notep, va_list ap)
{
    return ExpandErrorArgumentsHelper(cx, callback, userRef, errorNumber,
                                      messageArgs, argumentsType, notep, ap);
}

bool
js::ReportErrorNumberVA(JSContext* cx, unsigned flags, JSErrorCallback callback,
                        void* userRef, const unsigned errorNumber,
                        ErrorArgumentsType argumentsType, va_list ap)
{
    JSErrorReport report;
    bool warning;

    if (checkReportFlags(cx, &flags))
        return true;
    warning = JSREPORT_IS_WARNING(flags);

    report.flags = flags;
    report.errorNumber = errorNumber;
    PopulateReportBlame(cx, &report);

    if (!ExpandErrorArgumentsVA(cx, callback, userRef, errorNumber,
                                nullptr, argumentsType, &report, ap)) {
        return false;
    }

    ReportError(cx, &report, callback, userRef);

    return warning;
}

static bool
ExpandErrorArguments(JSContext* cx, JSErrorCallback callback,
                     void* userRef, const unsigned errorNumber,
                     const char16_t** messageArgs,
                     ErrorArgumentsType argumentsType,
                     JSErrorReport* reportp, ...)
{
    va_list ap;
    va_start(ap, reportp);
    bool expanded = js::ExpandErrorArgumentsVA(cx, callback, userRef, errorNumber,
                                               messageArgs, argumentsType, reportp, ap);
    va_end(ap);
    return expanded;
}

bool
js::ReportErrorNumberUCArray(JSContext* cx, unsigned flags, JSErrorCallback callback,
                             void* userRef, const unsigned errorNumber,
                             const char16_t** args)
{
    if (checkReportFlags(cx, &flags))
        return true;
    bool warning = JSREPORT_IS_WARNING(flags);

    JSErrorReport report;
    report.flags = flags;
    report.errorNumber = errorNumber;
    PopulateReportBlame(cx, &report);

    if (!ExpandErrorArguments(cx, callback, userRef, errorNumber,
                              args, ArgumentsAreUnicode, &report))
    {
        return false;
    }

    ReportError(cx, &report, callback, userRef);

    return warning;
}

bool
js::ReportIsNotDefined(JSContext* cx, HandleId id)
{
    JSAutoByteString printable;
    if (ValueToPrintable(cx, IdToValue(id), &printable)) {
        JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_NOT_DEFINED,
                                   printable.ptr());
    }
    return false;
}

bool
js::ReportIsNotDefined(JSContext* cx, HandlePropertyName name)
{
    RootedId id(cx, NameToId(name));
    return ReportIsNotDefined(cx, id);
}

bool
js::ReportIsNullOrUndefined(JSContext* cx, int spindex, HandleValue v,
                            HandleString fallback)
{
    bool ok;

    UniqueChars bytes = DecompileValueGenerator(cx, spindex, v, fallback);
    if (!bytes)
        return false;

    if (strcmp(bytes.get(), js_undefined_str) == 0 ||
        strcmp(bytes.get(), js_null_str) == 0) {
        ok = JS_ReportErrorFlagsAndNumberLatin1(cx, JSREPORT_ERROR,
                                                GetErrorMessage, nullptr,
                                                JSMSG_NO_PROPERTIES,
                                                bytes.get());
    } else if (v.isUndefined()) {
        ok = JS_ReportErrorFlagsAndNumberLatin1(cx, JSREPORT_ERROR,
                                                GetErrorMessage, nullptr,
                                                JSMSG_UNEXPECTED_TYPE,
                                                bytes.get(), js_undefined_str);
    } else {
        MOZ_ASSERT(v.isNull());
        ok = JS_ReportErrorFlagsAndNumberLatin1(cx, JSREPORT_ERROR,
                                                GetErrorMessage, nullptr,
                                                JSMSG_UNEXPECTED_TYPE,
                                                bytes.get(), js_null_str);
    }

    return ok;
}

void
js::ReportMissingArg(JSContext* cx, HandleValue v, unsigned arg)
{
    char argbuf[11];
    UniqueChars bytes;

    SprintfLiteral(argbuf, "%u", arg);
    if (IsFunctionObject(v)) {
        RootedAtom name(cx, v.toObject().as<JSFunction>().explicitName());
        bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, v, name);
        if (!bytes)
            return;
    }
    JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr,
                               JSMSG_MISSING_FUN_ARG,
                               argbuf, bytes ? bytes.get() : "");
}

bool
js::ReportValueErrorFlags(JSContext* cx, unsigned flags, const unsigned errorNumber,
                          int spindex, HandleValue v, HandleString fallback,
                          const char* arg1, const char* arg2)
{
    UniqueChars bytes;
    bool ok;

    MOZ_ASSERT(js_ErrorFormatString[errorNumber].argCount >= 1);
    MOZ_ASSERT(js_ErrorFormatString[errorNumber].argCount <= 3);
    bytes = DecompileValueGenerator(cx, spindex, v, fallback);
    if (!bytes)
        return false;

    ok = JS_ReportErrorFlagsAndNumberLatin1(cx, flags, GetErrorMessage, nullptr, errorNumber,
                                            bytes.get(), arg1, arg2);
    return ok;
}

JSObject*
js::CreateErrorNotesArray(JSContext* cx, JSErrorReport* report)
{
    RootedArrayObject notesArray(cx, NewDenseEmptyArray(cx));
    if (!notesArray)
        return nullptr;

    if (!report->notes)
        return notesArray;

    for (auto&& note : *report->notes) {
        RootedPlainObject noteObj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!noteObj)
            return nullptr;

        RootedString messageStr(cx, note->newMessageString(cx));
        if (!messageStr)
            return nullptr;
        RootedValue messageVal(cx, StringValue(messageStr));
        if (!DefineDataProperty(cx, noteObj, cx->names().message, messageVal))
            return nullptr;

        RootedValue filenameVal(cx);
        if (note->filename) {
            RootedString filenameStr(cx, NewStringCopyZ<CanGC>(cx, note->filename));
            if (!filenameStr)
                return nullptr;
            filenameVal = StringValue(filenameStr);
        }
        if (!DefineDataProperty(cx, noteObj, cx->names().fileName, filenameVal))
            return nullptr;

        RootedValue linenoVal(cx, Int32Value(note->lineno));
        if (!DefineDataProperty(cx, noteObj, cx->names().lineNumber, linenoVal))
            return nullptr;
        RootedValue columnVal(cx, Int32Value(note->column));
        if (!DefineDataProperty(cx, noteObj, cx->names().columnNumber, columnVal))
            return nullptr;

        if (!NewbornArrayPush(cx, notesArray, ObjectValue(*noteObj)))
            return nullptr;
    }

    return notesArray;
}

const JSErrorFormatString js_ErrorFormatString[JSErr_Limit] = {
#define MSG_DEF(name, count, exception, format) \
    { #name, format, count, exception } ,
#include "js.msg"
#undef MSG_DEF
};

JS_FRIEND_API(const JSErrorFormatString*)
js::GetErrorMessage(void* userRef, const unsigned errorNumber)
{
    if (errorNumber > 0 && errorNumber < JSErr_Limit)
        return &js_ErrorFormatString[errorNumber];
    return nullptr;
}

void
JSContext::recoverFromOutOfMemory()
{
    if (helperThread()) {
        // Keep in sync with addPendingOutOfMemory.
        if (ParseTask* task = helperThread()->parseTask())
            task->outOfMemory = false;
    } else {
        if (isExceptionPending()) {
            MOZ_ASSERT(isThrowingOutOfMemory());
            clearPendingException();
        }
    }
}

static bool
InternalEnqueuePromiseJobCallback(JSContext* cx, JS::HandleObject job,
                                  JS::HandleObject allocationSite,
                                  JS::HandleObject incumbentGlobal, void* data)
{
    MOZ_ASSERT(job);
    return cx->jobQueue->append(job);
}

namespace {
class MOZ_STACK_CLASS ReportExceptionClosure : public ScriptEnvironmentPreparer::Closure
{
  public:
    explicit ReportExceptionClosure(HandleValue exn)
        : exn_(exn)
    {
    }

    bool operator()(JSContext* cx) override
    {
        cx->setPendingException(exn_);
        return false;
    }

  private:
    HandleValue exn_;
};
} // anonymous namespace

JS_FRIEND_API(bool)
js::UseInternalJobQueues(JSContext* cx, bool cooperative)
{
    // Internal job queue handling must be set up very early. Self-hosting
    // initialization is as good a marker for that as any.
    MOZ_RELEASE_ASSERT(cooperative || !cx->runtime()->hasInitializedSelfHosting(),
                       "js::UseInternalJobQueues must be called early during runtime startup.");
    MOZ_ASSERT(!cx->jobQueue);
    auto* queue = js_new<PersistentRooted<JobQueue>>(cx, JobQueue(SystemAllocPolicy()));
    if (!queue)
        return false;

    cx->jobQueue = queue;

    if (!cooperative)
        cx->runtime()->offThreadPromiseState.ref().initInternalDispatchQueue();
    MOZ_ASSERT(cx->runtime()->offThreadPromiseState.ref().initialized());

    JS::SetEnqueuePromiseJobCallback(cx, InternalEnqueuePromiseJobCallback);

    return true;
}

JS_FRIEND_API(bool)
js::EnqueueJob(JSContext* cx, JS::HandleObject job)
{
    MOZ_ASSERT(cx->jobQueue);
    if (!cx->jobQueue->append(job)) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

JS_FRIEND_API(void)
js::StopDrainingJobQueue(JSContext* cx)
{
    MOZ_ASSERT(cx->jobQueue);
    cx->stopDrainingJobQueue = true;
}

JS_FRIEND_API(void)
js::RunJobs(JSContext* cx)
{
    MOZ_ASSERT(cx->jobQueue);

    if (cx->drainingJobQueue || cx->stopDrainingJobQueue)
        return;

    while (true) {
        cx->runtime()->offThreadPromiseState.ref().internalDrain(cx);

        // It doesn't make sense for job queue draining to be reentrant. At the
        // same time we don't want to assert against it, because that'd make
        // drainJobQueue unsafe for fuzzers. We do want fuzzers to test this,
        // so we simply ignore nested calls of drainJobQueue.
        cx->drainingJobQueue = true;

        RootedObject job(cx);
        JS::HandleValueArray args(JS::HandleValueArray::empty());
        RootedValue rval(cx);

        // Execute jobs in a loop until we've reached the end of the queue.
        // Since executing a job can trigger enqueuing of additional jobs,
        // it's crucial to re-check the queue length during each iteration.
        for (size_t i = 0; i < cx->jobQueue->length(); i++) {
            // A previous job might have set this flag. E.g., the js shell
            // sets it if the `quit` builtin function is called.
            if (cx->stopDrainingJobQueue)
                break;

            job = cx->jobQueue->get()[i];

            // It's possible that queue draining was interrupted prematurely,
            // leaving the queue partly processed. In that case, slots for
            // already-executed entries will contain nullptrs, which we should
            // just skip.
            if (!job)
                continue;

            cx->jobQueue->get()[i] = nullptr;
            AutoCompartment ac(cx, job);
            {
                if (!JS::Call(cx, UndefinedHandleValue, job, args, &rval)) {
                    // Nothing we can do about uncatchable exceptions.
                    if (!cx->isExceptionPending())
                        continue;
                    RootedValue exn(cx);
                    if (cx->getPendingException(&exn)) {
                        /*
                         * Clear the exception, because
                         * PrepareScriptEnvironmentAndInvoke will assert that we don't
                         * have one.
                         */
                        cx->clearPendingException();
                        ReportExceptionClosure reportExn(exn);
                        PrepareScriptEnvironmentAndInvoke(cx, cx->global(), reportExn);
                    }
                }
            }
        }

        cx->drainingJobQueue = false;

        if (cx->stopDrainingJobQueue) {
            cx->stopDrainingJobQueue = false;
            break;
        }

        cx->jobQueue->clear();

        // It's possible a job added a new off-thread promise task.
        if (!cx->runtime()->offThreadPromiseState.ref().internalHasPending())
            break;
    }
}

JS::Error JSContext::reportedError;
JS::OOM JSContext::reportedOOM;

mozilla::GenericErrorResult<OOM&>
JSContext::alreadyReportedOOM()
{
#ifdef DEBUG
    if (helperThread()) {
        // Keep in sync with addPendingOutOfMemory.
        if (ParseTask* task = helperThread()->parseTask())
            MOZ_ASSERT(task->outOfMemory);
    } else {
        MOZ_ASSERT(isThrowingOutOfMemory());
    }
#endif
    return mozilla::Err(reportedOOM);
}

mozilla::GenericErrorResult<JS::Error&>
JSContext::alreadyReportedError()
{
#ifdef DEBUG
    if (!helperThread())
        MOZ_ASSERT(isExceptionPending());
#endif
    return mozilla::Err(reportedError);
}

JSContext::JSContext(JSRuntime* runtime, const JS::ContextOptions& options)
  : runtime_(runtime),
    kind_(ContextKind::Background),
    threadNative_(0),
    helperThread_(nullptr),
    options_(options),
    arenas_(nullptr),
    enterCompartmentDepth_(0),
    jitActivation(nullptr),
    activation_(nullptr),
    profilingActivation_(nullptr),
    nativeStackBase(GetNativeStackBase()),
    entryMonitor(nullptr),
    noExecuteDebuggerTop(nullptr),
    activityCallback(nullptr),
    activityCallbackArg(nullptr),
    requestDepth(0),
#ifdef DEBUG
    checkRequestDepth(0),
    inUnsafeCallWithABI(false),
    hasAutoUnsafeCallWithABI(false),
#endif
#ifdef JS_SIMULATOR
    simulator_(nullptr),
#endif
#ifdef JS_TRACE_LOGGING
    traceLogger(nullptr),
#endif
    autoFlushICache_(nullptr),
    dtoaState(nullptr),
    heapState(JS::HeapState::Idle),
    suppressGC(0),
#ifdef DEBUG
    ionCompiling(false),
    ionCompilingSafeForMinorGC(false),
    performingGC(false),
    gcSweeping(false),
    gcHelperStateThread(false),
    isTouchingGrayThings(false),
    noGCOrAllocationCheck(0),
    noNurseryAllocationCheck(0),
    disableStrictProxyCheckingCount(0),
#endif
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    runningOOMTest(false),
#endif
    enableAccessValidation(false),
    inUnsafeRegion(0),
    generationalDisabled(0),
    compactingDisabledCount(0),
    keepAtoms(0),
    suppressProfilerSampling(false),
    tempLifoAlloc_((size_t)TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    debuggerMutations(0),
    ionPcScriptCache(nullptr),
    throwing(false),
    overRecursed_(false),
    propagatingForcedReturn_(false),
    liveVolatileJitFrameIter_(nullptr),
    reportGranularity(JS_DEFAULT_JITREPORT_GRANULARITY),
    resolvingList(nullptr),
#ifdef DEBUG
    enteredPolicy(nullptr),
#endif
    generatingError(false),
    cycleDetectorVector_(this),
    data(nullptr),
    outstandingRequests(0),
    jitIsBroken(false),
    asyncCauseForNewActivations(nullptr),
    asyncCallIsExplicit(false),
    interruptCallbackDisabled(false),
    interrupt_(false),
    interruptRegExpJit_(false),
    handlingJitInterrupt_(false),
    osrTempData_(nullptr),
    ionReturnOverride_(MagicValue(JS_ARG_POISON)),
    jitStackLimit(UINTPTR_MAX),
    jitStackLimitNoInterrupt(UINTPTR_MAX),
    getIncumbentGlobalCallback(nullptr),
    enqueuePromiseJobCallback(nullptr),
    enqueuePromiseJobCallbackData(nullptr),
    jobQueue(nullptr),
    drainingJobQueue(false),
    stopDrainingJobQueue(false),
    promiseRejectionTrackerCallback(nullptr),
    promiseRejectionTrackerCallbackData(nullptr)
{
    MOZ_ASSERT(static_cast<JS::RootingContext*>(this) ==
               JS::RootingContext::get(this));

    MOZ_ASSERT(!TlsContext.get());
    TlsContext.set(this);

    for (size_t i = 0; i < mozilla::ArrayLength(nativeStackQuota); i++)
        nativeStackQuota[i] = 0;
}

JSContext::~JSContext()
{
    // Clear the ContextKind first, so that ProtectedData checks will allow us to
    // destroy this context even if the runtime is already gone.
    kind_ = ContextKind::Background;

#ifdef XP_WIN
    if (threadNative_)
        CloseHandle((HANDLE)threadNative_.ref());
#endif

    /* Free the stuff hanging off of cx. */
    MOZ_ASSERT(!resolvingList);

    js_delete(ionPcScriptCache.ref());

    if (dtoaState)
        DestroyDtoaState(dtoaState);

    fx.destroyInstance();
    freeOsrTempData();

#ifdef JS_SIMULATOR
    js::jit::Simulator::Destroy(simulator_);
#endif

#ifdef JS_TRACE_LOGGING
    if (traceLogger)
        DestroyTraceLogger(traceLogger);
#endif

    MOZ_ASSERT(TlsContext.get() == this);
    TlsContext.set(nullptr);
}

void
JSContext::setRuntime(JSRuntime* rt)
{
    MOZ_ASSERT(!resolvingList);
    MOZ_ASSERT(!compartment());
    MOZ_ASSERT(!activation());
    MOZ_ASSERT(!unwrappedException_.ref().initialized());
    MOZ_ASSERT(!asyncStackForNewActivations_.ref().initialized());

    runtime_ = rt;
}

bool
JSContext::getPendingException(MutableHandleValue rval)
{
    MOZ_ASSERT(throwing);
    rval.set(unwrappedException());
    if (IsAtomsCompartment(compartment()))
        return true;
    bool wasOverRecursed = overRecursed_;
    clearPendingException();
    if (!compartment()->wrap(this, rval))
        return false;
    assertSameCompartment(this, rval);
    setPendingException(rval);
    overRecursed_ = wasOverRecursed;
    return true;
}

bool
JSContext::isThrowingOutOfMemory()
{
    return throwing && unwrappedException() == StringValue(names().outOfMemory);
}

bool
JSContext::isClosingGenerator()
{
    return throwing && unwrappedException().isMagic(JS_GENERATOR_CLOSING);
}

bool
JSContext::isThrowingDebuggeeWouldRun()
{
    return throwing &&
           unwrappedException().isObject() &&
           unwrappedException().toObject().is<ErrorObject>() &&
           unwrappedException().toObject().as<ErrorObject>().type() == JSEXN_DEBUGGEEWOULDRUN;
}

static bool
ComputeIsJITBroken()
{
#if !defined(ANDROID)
    return false;
#else  // ANDROID
    if (getenv("JS_IGNORE_JIT_BROKENNESS")) {
        return false;
    }

    std::string line;

    // Check for the known-bad kernel version (2.6.29).
    std::ifstream osrelease("/proc/sys/kernel/osrelease");
    std::getline(osrelease, line);
    __android_log_print(ANDROID_LOG_INFO, "Gecko", "Detected osrelease `%s'",
                        line.c_str());

    if (line.npos == line.find("2.6.29")) {
        // We're using something other than 2.6.29, so the JITs should work.
        __android_log_print(ANDROID_LOG_INFO, "Gecko", "JITs are not broken");
        return false;
    }

    // We're using 2.6.29, and this causes trouble with the JITs on i9000.
    line = "";
    bool broken = false;
    std::ifstream cpuinfo("/proc/cpuinfo");
    do {
        if (0 == line.find("Hardware")) {
            static const char* const blacklist[] = {
                "SCH-I400",     // Samsung Continuum
                "SGH-T959",     // Samsung i9000, Vibrant device
                "SGH-I897",     // Samsung i9000, Captivate device
                "SCH-I500",     // Samsung i9000, Fascinate device
                "SPH-D700",     // Samsung i9000, Epic device
                "GT-I9000",     // Samsung i9000, UK/Europe device
                nullptr
            };
            for (const char* const* hw = &blacklist[0]; *hw; ++hw) {
                if (line.npos != line.find(*hw)) {
                    __android_log_print(ANDROID_LOG_INFO, "Gecko",
                                        "Blacklisted device `%s'", *hw);
                    broken = true;
                    break;
                }
            }
            break;
        }
        std::getline(cpuinfo, line);
    } while(!cpuinfo.fail() && !cpuinfo.eof());

    __android_log_print(ANDROID_LOG_INFO, "Gecko", "JITs are %sbroken",
                        broken ? "" : "not ");

    return broken;
#endif  // ifndef ANDROID
}

static bool
IsJITBrokenHere()
{
    static bool computedIsBroken = false;
    static bool isBroken = false;
    if (!computedIsBroken) {
        isBroken = ComputeIsJITBroken();
        computedIsBroken = true;
    }
    return isBroken;
}

void
JSContext::updateJITEnabled()
{
    jitIsBroken = IsJITBrokenHere();
}

size_t
JSContext::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    /*
     * There are other JSContext members that could be measured; the following
     * ones have been found by DMD to be worth measuring.  More stuff may be
     * added later.
     */
    return cycleDetectorVector().sizeOfExcludingThis(mallocSizeOf);
}

void
JSContext::trace(JSTracer* trc)
{
    cycleDetectorVector().trace(trc);
    geckoProfiler().trace(trc);

    if (trc->isMarkingTracer() && compartment_)
        compartment_->mark();
}

void*
JSContext::stackLimitAddressForJitCode(JS::StackKind kind)
{
#ifdef JS_SIMULATOR
    return addressOfSimulatorStackLimit();
#else
    return stackLimitAddress(kind);
#endif
}

uintptr_t
JSContext::stackLimitForJitCode(JS::StackKind kind)
{
#ifdef JS_SIMULATOR
    return simulator()->stackLimit();
#else
    return stackLimit(kind);
#endif
}

void
JSContext::resetJitStackLimit()
{
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

void
JSContext::initJitStackLimit()
{
    resetJitStackLimit();
}

void
JSContext::updateMallocCounter(size_t nbytes)
{
    if (!zone()) {
        runtime()->updateMallocCounter(nbytes);
        return;
    }

    zone()->updateMallocCounter(nbytes);
}

#ifdef DEBUG

JS::AutoCheckRequestDepth::AutoCheckRequestDepth(JSContext* cxArg)
  : cx(cxArg->helperThread() ? nullptr : cxArg)
{
    if (cx) {
        MOZ_ASSERT(cx->requestDepth || JS::CurrentThreadIsHeapBusy());
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
        cx->checkRequestDepth++;
    }
}

JS::AutoCheckRequestDepth::~AutoCheckRequestDepth()
{
    if (cx) {
        MOZ_ASSERT(cx->checkRequestDepth != 0);
        cx->checkRequestDepth--;
    }
}

#endif

#ifdef JS_CRASH_DIAGNOSTICS
void
CompartmentChecker::check(InterpreterFrame* fp)
{
    if (fp)
        check(fp->environmentChain());
}

void
CompartmentChecker::check(AbstractFramePtr frame)
{
    if (frame)
        check(frame.environmentChain());
}
#endif

void
AutoEnterOOMUnsafeRegion::crash(const char* reason)
{
    char msgbuf[1024];
    js::NoteIntentionalCrash();
    SprintfLiteral(msgbuf, "[unhandlable oom] %s", reason);
    MOZ_ReportAssertionFailure(msgbuf, __FILE__, __LINE__);
    MOZ_CRASH();
}

AutoEnterOOMUnsafeRegion::AnnotateOOMAllocationSizeCallback
AutoEnterOOMUnsafeRegion::annotateOOMSizeCallback = nullptr;

void
AutoEnterOOMUnsafeRegion::crash(size_t size, const char* reason)
{
    {
        JS::AutoSuppressGCAnalysis suppress;
        if (annotateOOMSizeCallback)
            annotateOOMSizeCallback(size);
    }
    crash(reason);
}

#ifdef DEBUG
AutoUnsafeCallWithABI::AutoUnsafeCallWithABI()
  : cx_(TlsContext.get()),
    nested_(cx_->hasAutoUnsafeCallWithABI),
    nogc(cx_)
{
    cx_->hasAutoUnsafeCallWithABI = true;
}

AutoUnsafeCallWithABI::~AutoUnsafeCallWithABI()
{
    MOZ_ASSERT(cx_->hasAutoUnsafeCallWithABI);
    if (!nested_) {
        cx_->hasAutoUnsafeCallWithABI = false;
        cx_->inUnsafeCallWithABI = false;
    }
}
#endif
