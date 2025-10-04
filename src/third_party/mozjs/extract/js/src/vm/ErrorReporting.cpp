/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ErrorReporting.h"

#include <stdarg.h>
#include <utility>

#include "jsexn.h"
#include "jsfriendapi.h"

#include "frontend/FrontendContext.h"  // AutoReportFrontendContext
#include "js/CharacterEncoding.h"      // JS::ConstUTF8CharsZ
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOrigin, JS::TaggedColumnNumberOneOrigin
#include "js/ErrorReport.h"           // JSErrorBase
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printf.h"                // JS_vsmprintf
#include "js/Warnings.h"              // JS::WarningReporter
#include "vm/FrameIter.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"

using namespace js;

using JS::HandleObject;
using JS::HandleValue;
using JS::UniqueTwoByteChars;

void js::CallWarningReporter(JSContext* cx, JSErrorReport* reportp) {
  MOZ_ASSERT(reportp->isWarning());

  if (JS::WarningReporter warningReporter = cx->runtime()->warningReporter) {
    warningReporter(cx, reportp);
  }
}

bool js::CompileError::throwError(JSContext* cx) {
  if (isWarning()) {
    CallWarningReporter(cx, this);
    return true;
  }

  // If there's a runtime exception type associated with this error
  // number, set that as the pending exception.  For errors occurring at
  // compile time, this is very likely to be a JSEXN_SYNTAXERR.
  return ErrorToException(cx, this, nullptr, nullptr);
}

bool js::ReportExceptionClosure::operator()(JSContext* cx) {
  cx->setPendingException(exn_, ShouldCaptureStack::Always);
  return false;
}

bool js::ReportCompileWarning(FrontendContext* fc, ErrorMetadata&& metadata,
                              UniquePtr<JSErrorNotes> notes,
                              unsigned errorNumber, va_list* args) {
  // On the main thread, report the error immediately. When compiling off
  // thread, save the error so that the thread finishing the parse can report
  // it later.
  CompileError err;

  err.notes = std::move(notes);
  err.isWarning_ = true;
  err.errorNumber = errorNumber;

  err.filename = JS::ConstUTF8CharsZ(metadata.filename);
  err.lineno = metadata.lineNumber;
  err.column = metadata.columnNumber;
  err.isMuted = metadata.isMuted;

  if (UniqueTwoByteChars lineOfContext = std::move(metadata.lineOfContext)) {
    err.initOwnedLinebuf(lineOfContext.release(), metadata.lineLength,
                         metadata.tokenOffset);
  }

  if (!ExpandErrorArgumentsVA(fc, GetErrorMessage, nullptr, errorNumber,
                              ArgumentsAreLatin1, &err, *args)) {
    return false;
  }

  return fc->reportWarning(std::move(err));
}

static void ReportCompileErrorImpl(FrontendContext* fc,
                                   js::ErrorMetadata&& metadata,
                                   js::UniquePtr<JSErrorNotes> notes,
                                   unsigned errorNumber, va_list* args,
                                   ErrorArgumentsType argumentsType) {
  js::CompileError err;

  err.notes = std::move(notes);
  err.isWarning_ = false;
  err.errorNumber = errorNumber;

  err.filename = JS::ConstUTF8CharsZ(metadata.filename);
  err.lineno = metadata.lineNumber;
  err.column = metadata.columnNumber;
  err.isMuted = metadata.isMuted;

  if (UniqueTwoByteChars lineOfContext = std::move(metadata.lineOfContext)) {
    err.initOwnedLinebuf(lineOfContext.release(), metadata.lineLength,
                         metadata.tokenOffset);
  }

  if (!js::ExpandErrorArgumentsVA(fc, js::GetErrorMessage, nullptr, errorNumber,
                                  argumentsType, &err, *args)) {
    return;
  }

  fc->reportError(std::move(err));
}

void js::ReportCompileErrorLatin1(FrontendContext* fc, ErrorMetadata&& metadata,
                                  UniquePtr<JSErrorNotes> notes,
                                  unsigned errorNumber, ...) {
  va_list args;
  va_start(args, errorNumber);
  ReportCompileErrorLatin1VA(fc, std::move(metadata), std::move(notes),
                             errorNumber, &args);
  va_end(args);
}

void js::ReportCompileErrorUTF8(FrontendContext* fc, ErrorMetadata&& metadata,
                                UniquePtr<JSErrorNotes> notes,
                                unsigned errorNumber, ...) {
  va_list args;
  va_start(args, errorNumber);
  ReportCompileErrorUTF8VA(fc, std::move(metadata), std::move(notes),
                           errorNumber, &args);
  va_end(args);
}

void js::ReportCompileErrorLatin1VA(FrontendContext* fc,
                                    ErrorMetadata&& metadata,
                                    UniquePtr<JSErrorNotes> notes,
                                    unsigned errorNumber, va_list* args) {
  ReportCompileErrorImpl(fc, std::move(metadata), std::move(notes), errorNumber,
                         args, ArgumentsAreLatin1);
}

void js::ReportCompileErrorUTF8VA(FrontendContext* fc, ErrorMetadata&& metadata,
                                  UniquePtr<JSErrorNotes> notes,
                                  unsigned errorNumber, va_list* args) {
  ReportCompileErrorImpl(fc, std::move(metadata), std::move(notes), errorNumber,
                         args, ArgumentsAreUTF8);
}

void js::ReportErrorToGlobal(JSContext* cx, Handle<GlobalObject*> global,
                             HandleValue error) {
  MOZ_ASSERT(!cx->isExceptionPending());
#ifdef DEBUG
  // No assertSameCompartment version that doesn't take JSContext...
  if (error.isObject()) {
    AssertSameCompartment(global, &error.toObject());
  }
#endif  // DEBUG
  js::ReportExceptionClosure report(error);
  PrepareScriptEnvironmentAndInvoke(cx, global, report);
}

static bool ReportError(JSContext* cx, JSErrorReport* reportp,
                        JSErrorCallback callback, void* userRef) {
  if (reportp->isWarning()) {
    CallWarningReporter(cx, reportp);
    return true;
  }

  // Check the error report, and set a JavaScript-catchable exception
  // if the error is defined to have an associated exception.
  return ErrorToException(cx, reportp, callback, userRef);
}

/*
 * The given JSErrorReport object have been zeroed and must not outlive
 * cx->fp() (otherwise owned fields may become invalid).
 */
static void PopulateReportBlame(JSContext* cx, JSErrorReport* report) {
  JS::Realm* realm = cx->realm();
  if (!realm) {
    return;
  }

  /*
   * Walk stack until we find a frame that is associated with a non-builtin
   * rather than a builtin frame and which we're allowed to know about.
   */
  NonBuiltinFrameIter iter(cx, realm->principals());
  if (iter.done()) {
    return;
  }

  report->filename = JS::ConstUTF8CharsZ(iter.filename());
  if (iter.hasScript()) {
    report->sourceId = iter.script()->scriptSource()->id();
  }
  JS::TaggedColumnNumberOneOrigin column;
  report->lineno = iter.computeLine(&column);
  report->column = JS::ColumnNumberOneOrigin(column.oneOriginValue());
  report->isMuted = iter.mutedErrors();
}

class MOZ_RAII AutoMessageArgs {
  size_t totalLength_;
  /* only {0} thru {9} supported */
  mozilla::Array<const char*, JS::MaxNumErrorArguments> args_;
  mozilla::Array<size_t, JS::MaxNumErrorArguments> lengths_;
  uint16_t count_;
  bool allocatedElements_ : 1;

 public:
  AutoMessageArgs() : totalLength_(0), count_(0), allocatedElements_(false) {
    PodArrayZero(args_);
  }

  ~AutoMessageArgs() {
    /* free the arguments only if we allocated them */
    if (allocatedElements_) {
      uint16_t i = 0;
      while (i < count_) {
        if (args_[i]) {
          js_free((void*)args_[i]);
        }
        i++;
      }
    }
  }

  const char* args(size_t i) const {
    MOZ_ASSERT(i < count_);
    return args_[i];
  }

  size_t totalLength() const { return totalLength_; }

  size_t lengths(size_t i) const {
    MOZ_ASSERT(i < count_);
    return lengths_[i];
  }

  uint16_t count() const { return count_; }

  /* Gather the arguments into an array, and accumulate their sizes.
   *
   * We could template on the type of argsArg, but we're already trusting people
   * to do the right thing with varargs, so might as well trust them on this
   * part too.  Upstream consumers do assert that it's the right thing.  Also,
   * if argsArg were strongly typed we'd still need casting below for this to
   * compile, because typeArg is not known at compile-time here.
   */
  template <typename Allocator>
  bool init(Allocator* alloc, void* argsArg, uint16_t countArg,
            ErrorArgumentsType typeArg, va_list ap) {
    MOZ_ASSERT(countArg > 0);

    count_ = countArg;

    for (uint16_t i = 0; i < count_; i++) {
      switch (typeArg) {
        case ArgumentsAreASCII:
        case ArgumentsAreUTF8: {
          const char* c = argsArg ? static_cast<const char**>(argsArg)[i]
                                  : va_arg(ap, const char*);
          args_[i] = c;
          MOZ_ASSERT_IF(typeArg == ArgumentsAreASCII,
                        JS::StringIsASCII(args_[i]));
          lengths_[i] = strlen(args_[i]);
          break;
        }
        case ArgumentsAreLatin1: {
          MOZ_ASSERT(!argsArg);
          const Latin1Char* latin1 = va_arg(ap, Latin1Char*);
          size_t len = strlen(reinterpret_cast<const char*>(latin1));
          mozilla::Range<const Latin1Char> range(latin1, len);
          char* utf8 = JS::CharsToNewUTF8CharsZ(alloc, range).c_str();
          if (!utf8) {
            return false;
          }

          args_[i] = utf8;
          lengths_[i] = strlen(utf8);
          allocatedElements_ = true;
          break;
        }
        case ArgumentsAreUnicode: {
          const char16_t* uc = argsArg
                                   ? static_cast<const char16_t**>(argsArg)[i]
                                   : va_arg(ap, const char16_t*);
          size_t len = js_strlen(uc);
          mozilla::Range<const char16_t> range(uc, len);
          char* utf8 = JS::CharsToNewUTF8CharsZ(alloc, range).c_str();
          if (!utf8) {
            return false;
          }

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
 *
 * messageArgs is a `const char**` or a `const char16_t**` but templating on
 * that is not worth it here because AutoMessageArgs takes a void* anyway, and
 * using void* here simplifies our callers a bit.
 */
template <typename T>
static bool ExpandErrorArgumentsHelper(FrontendContext* fc,
                                       JSErrorCallback callback, void* userRef,
                                       const unsigned errorNumber,
                                       void* messageArgs,
                                       ErrorArgumentsType argumentsType,
                                       T* reportp, va_list ap) {
  const JSErrorFormatString* efs;

  if (!callback) {
    callback = GetErrorMessage;
  }

  efs = fc->gcSafeCallback(callback, userRef, errorNumber);

  if (efs) {
    if constexpr (std::is_same_v<T, JSErrorReport>) {
      reportp->exnType = efs->exnType;
    }

    MOZ_ASSERT(reportp->errorNumber == errorNumber);
    reportp->errorMessageName = efs->name;

    MOZ_ASSERT_IF(argumentsType == ArgumentsAreASCII,
                  JS::StringIsASCII(efs->format));

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
        if (!args.init(fc->getAllocator(), messageArgs, argCount, argumentsType,
                       ap)) {
          return false;
        }

        expandedLength = len - (3 * args.count()) /* exclude the {n} */
                         + args.totalLength();

        /*
         * Note - the above calculation assumes that each argument
         * is used once and only once in the expansion !!!
         */
        char* utf8 = out =
            fc->getAllocator()->pod_malloc<char>(expandedLength + 1);
        if (!out) {
          return false;
        }

        fmt = efs->format;
        while (*fmt) {
          if (*fmt == '{') {
            if (mozilla::IsAsciiDigit(fmt[1])) {
              int d = AsciiDigitToNumber(fmt[1]);
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
      if (efs->format) {
        reportp->initBorrowedMessage(efs->format);
      }
    }
  }
  if (!reportp->message()) {
    /* where's the right place for this ??? */
    const char* defaultErrorMessage =
        "No error message available for error number %d";
    size_t nbytes = strlen(defaultErrorMessage) + 16;
    char* message = fc->getAllocator()->pod_malloc<char>(nbytes);
    if (!message) {
      return false;
    }
    snprintf(message, nbytes, defaultErrorMessage, errorNumber);
    reportp->initOwnedMessage(message);
  }
  return true;
}

bool js::ExpandErrorArgumentsVA(FrontendContext* fc, JSErrorCallback callback,
                                void* userRef, const unsigned errorNumber,
                                const char16_t** messageArgs,
                                ErrorArgumentsType argumentsType,
                                JSErrorReport* reportp, va_list ap) {
  MOZ_ASSERT(argumentsType == ArgumentsAreUnicode);
  return ExpandErrorArgumentsHelper(fc, callback, userRef, errorNumber,
                                    messageArgs, argumentsType, reportp, ap);
}

bool js::ExpandErrorArgumentsVA(FrontendContext* fc, JSErrorCallback callback,
                                void* userRef, const unsigned errorNumber,
                                const char** messageArgs,
                                ErrorArgumentsType argumentsType,
                                JSErrorReport* reportp, va_list ap) {
  MOZ_ASSERT(argumentsType != ArgumentsAreUnicode);
  return ExpandErrorArgumentsHelper(fc, callback, userRef, errorNumber,
                                    messageArgs, argumentsType, reportp, ap);
}

bool js::ExpandErrorArgumentsVA(FrontendContext* fc, JSErrorCallback callback,
                                void* userRef, const unsigned errorNumber,
                                ErrorArgumentsType argumentsType,
                                JSErrorReport* reportp, va_list ap) {
  return ExpandErrorArgumentsHelper(fc, callback, userRef, errorNumber, nullptr,
                                    argumentsType, reportp, ap);
}

bool js::ExpandErrorArgumentsVA(FrontendContext* fc, JSErrorCallback callback,
                                void* userRef, const unsigned errorNumber,
                                const char16_t** messageArgs,
                                ErrorArgumentsType argumentsType,
                                JSErrorNotes::Note* notep, va_list ap) {
  return ExpandErrorArgumentsHelper(fc, callback, userRef, errorNumber,
                                    messageArgs, argumentsType, notep, ap);
}

bool js::ReportErrorNumberVA(JSContext* cx, IsWarning isWarning,
                             JSErrorCallback callback, void* userRef,
                             const unsigned errorNumber,
                             ErrorArgumentsType argumentsType, va_list ap) {
  JSErrorReport report;
  report.isWarning_ = isWarning == IsWarning::Yes;
  report.errorNumber = errorNumber;
  PopulateReportBlame(cx, &report);

  AutoReportFrontendContext fc(cx);
  if (!ExpandErrorArgumentsVA(&fc, callback, userRef, errorNumber,
                              argumentsType, &report, ap)) {
    return false;
  }

  if (!ReportError(cx, &report, callback, userRef)) {
    return false;
  }

  return report.isWarning();
}

template <typename CharT>
static bool ExpandErrorArguments(FrontendContext* fc, JSErrorCallback callback,
                                 void* userRef, const unsigned errorNumber,
                                 const CharT** messageArgs,
                                 js::ErrorArgumentsType argumentsType,
                                 JSErrorReport* reportp, ...) {
  va_list ap;
  va_start(ap, reportp);
  bool expanded =
      js::ExpandErrorArgumentsVA(fc, callback, userRef, errorNumber,
                                 messageArgs, argumentsType, reportp, ap);
  va_end(ap);
  return expanded;
}

template <js::ErrorArgumentsType argType, typename CharT>
static bool ReportErrorNumberArray(JSContext* cx, IsWarning isWarning,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   const CharT** args) {
  static_assert(
      (argType == ArgumentsAreUnicode && std::is_same_v<CharT, char16_t>) ||
          (argType != ArgumentsAreUnicode && std::is_same_v<CharT, char>),
      "Mismatch between character type and argument type");

  JSErrorReport report;
  report.isWarning_ = isWarning == IsWarning::Yes;
  report.errorNumber = errorNumber;
  PopulateReportBlame(cx, &report);

  AutoReportFrontendContext fc(cx);
  if (!ExpandErrorArguments(&fc, callback, userRef, errorNumber, args, argType,
                            &report)) {
    return false;
  }

  if (!ReportError(cx, &report, callback, userRef)) {
    return false;
  }

  return report.isWarning();
}

bool js::ReportErrorNumberUCArray(JSContext* cx, IsWarning isWarning,
                                  JSErrorCallback callback, void* userRef,
                                  const unsigned errorNumber,
                                  const char16_t** args) {
  return ReportErrorNumberArray<ArgumentsAreUnicode>(
      cx, isWarning, callback, userRef, errorNumber, args);
}

bool js::ReportErrorNumberUTF8Array(JSContext* cx, IsWarning isWarning,
                                    JSErrorCallback callback, void* userRef,
                                    const unsigned errorNumber,
                                    const char** args) {
  return ReportErrorNumberArray<ArgumentsAreUTF8>(cx, isWarning, callback,
                                                  userRef, errorNumber, args);
}

bool js::ReportErrorVA(JSContext* cx, IsWarning isWarning, const char* format,
                       js::ErrorArgumentsType argumentsType, va_list ap) {
  JSErrorReport report;

  UniqueChars message(JS_vsmprintf(format, ap));
  if (!message) {
    ReportOutOfMemory(cx);
    return false;
  }

  MOZ_ASSERT_IF(argumentsType == ArgumentsAreASCII,
                JS::StringIsASCII(message.get()));

  report.isWarning_ = isWarning == IsWarning::Yes;
  report.errorNumber = JSMSG_USER_DEFINED_ERROR;
  if (argumentsType == ArgumentsAreASCII || argumentsType == ArgumentsAreUTF8) {
    report.initOwnedMessage(message.release());
  } else {
    MOZ_ASSERT(argumentsType == ArgumentsAreLatin1);
    JS::Latin1Chars latin1(message.get(), strlen(message.get()));
    JS::UTF8CharsZ utf8(JS::CharsToNewUTF8CharsZ(cx, latin1));
    if (!utf8) {
      return false;
    }
    report.initOwnedMessage(reinterpret_cast<const char*>(utf8.get()));
  }
  PopulateReportBlame(cx, &report);

  if (!ReportError(cx, &report, nullptr, nullptr)) {
    return false;
  }

  return report.isWarning();
}

void js::MaybePrintAndClearPendingException(JSContext* cx) {
  if (!cx->isExceptionPending()) {
    return;
  }

  AutoClearPendingException acpe(cx);

  JS::ExceptionStack exnStack(cx);
  if (!JS::StealPendingExceptionStack(cx, &exnStack)) {
    fprintf(stderr, "error getting pending exception\n");
    return;
  }

  JS::ErrorReportBuilder report(cx);
  if (!report.init(cx, exnStack, JS::ErrorReportBuilder::WithSideEffects)) {
    fprintf(stderr, "out of memory initializing JS::ErrorReportBuilder\n");
    return;
  }

  MOZ_ASSERT(!report.report()->isWarning());
  JS::PrintError(stderr, report, true);
}
