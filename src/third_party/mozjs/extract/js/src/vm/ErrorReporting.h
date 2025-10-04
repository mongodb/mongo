/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorReporting_h
#define vm_ErrorReporting_h

#include <stdarg.h>

#include "jsfriendapi.h"  // for ScriptEnvironmentPreparer

#include "js/CharacterEncoding.h"  // JS::ConstUTF8CharsZ
#include "js/ColumnNumber.h"       // JS::ColumnNumberOneOrigin
#include "js/ErrorReport.h"        // for JSErrorNotes, JSErrorReport
#include "js/UniquePtr.h"          // for UniquePtr
#include "js/Utility.h"            // for UniqueTwoByteChars

namespace js {

class FrontendContext;

/**
 * Use this type instead of JSContext when the object is only used for its
 * ability to allocate memory (via its MallocProvider methods).
 */
using JSAllocator = JSContext;

/**
 * Metadata for a compilation error (or warning) at a particular offset, or at
 * no offset (i.e. with respect to a script overall).
 */
struct ErrorMetadata {
  // The file/URL where the error occurred.
  JS::ConstUTF8CharsZ filename;

  // The line and column numbers where the error occurred.  If the error
  // is with respect to the entire script and not with respect to a
  // particular location, these will both be zero.

  // Line number (1-origin).
  uint32_t lineNumber;

  // Column number in UTF-16 code units.
  JS::ColumnNumberOneOrigin columnNumber;

  // If the error occurs at a particular location, context surrounding the
  // location of the error: the line that contained the error, or a small
  // portion of it if the line is long.  (If the error occurs within a
  // regular expression, this context is based upon its pattern characters.)
  //
  // This information is provided on a best-effort basis: code populating
  // ErrorMetadata instances isn't obligated to supply this.
  JS::UniqueTwoByteChars lineOfContext;

  // If |lineOfContext| is provided, we show only a portion (a "window") of
  // the line around the erroneous token -- the first char in the token, plus
  // |lineOfContextRadius| chars before it and |lineOfContextRadius - 1|
  // chars after it.  This is because for a very long line, the full line is
  // (a) not that helpful, and (b) wastes a lot of memory.  See bug 634444.
  static constexpr size_t lineOfContextRadius = 60;

  // If |lineOfContext| is non-null, its length.
  size_t lineLength;

  // If |lineOfContext| is non-null, the offset within it of the token that
  // triggered the error.
  size_t tokenOffset;

  // Whether the error is "muted" because it derives from a cross-origin
  // load.  See the comment in TransitiveCompileOptions in jsapi.h for
  // details.
  bool isMuted;
};

class CompileError : public JSErrorReport {
 public:
  bool throwError(JSContext* cx);
};

class MOZ_STACK_CLASS ReportExceptionClosure final
    : public ScriptEnvironmentPreparer::Closure {
  JS::HandleValue exn_;

 public:
  explicit ReportExceptionClosure(JS::HandleValue exn) : exn_(exn) {}

  bool operator()(JSContext* cx) override;
};

/** Send a JSErrorReport to the warningReporter callback. */
extern void CallWarningReporter(JSContext* cx, JSErrorReport* report);

/**
 * Report a compile error during script processing prior to execution of the
 * script.
 */
extern void ReportCompileErrorLatin1VA(FrontendContext* fc,
                                       ErrorMetadata&& metadata,
                                       UniquePtr<JSErrorNotes> notes,
                                       unsigned errorNumber, va_list* args);

extern void ReportCompileErrorUTF8VA(FrontendContext* fc,
                                     ErrorMetadata&& metadata,
                                     UniquePtr<JSErrorNotes> notes,
                                     unsigned errorNumber, va_list* args);

extern void ReportCompileErrorLatin1(FrontendContext* fc,
                                     ErrorMetadata&& metadata,
                                     UniquePtr<JSErrorNotes> notes,
                                     unsigned errorNumber, ...);

extern void ReportCompileErrorUTF8(FrontendContext* fc,
                                   ErrorMetadata&& metadata,
                                   UniquePtr<JSErrorNotes> notes,
                                   unsigned errorNumber, ...);

/**
 * Report a compile warning during script processing prior to execution of the
 * script.  Returns true if the warning was successfully reported, false if an
 * error occurred.
 */
[[nodiscard]] extern bool ReportCompileWarning(FrontendContext* fc,
                                               ErrorMetadata&& metadata,
                                               UniquePtr<JSErrorNotes> notes,
                                               unsigned errorNumber,
                                               va_list* args);

class GlobalObject;

/**
 * Report the given error Value to the given global.  The JSContext is not
 * assumed to be in any particular realm, but the global and error are
 * expected to be same-compartment.
 */
extern void ReportErrorToGlobal(JSContext* cx,
                                JS::Handle<js::GlobalObject*> global,
                                JS::HandleValue error);

enum class IsWarning { No, Yes };

/**
 * Report an exception, using printf-style APIs to generate the error
 * message.
 */
extern bool ReportErrorVA(JSContext* cx, IsWarning isWarning,
                          const char* format, ErrorArgumentsType argumentsType,
                          va_list ap) MOZ_FORMAT_PRINTF(3, 0);

extern bool ReportErrorNumberVA(JSContext* cx, IsWarning isWarning,
                                JSErrorCallback callback, void* userRef,
                                const unsigned errorNumber,
                                ErrorArgumentsType argumentsType, va_list ap);

extern bool ReportErrorNumberUCArray(JSContext* cx, IsWarning isWarning,
                                     JSErrorCallback callback, void* userRef,
                                     const unsigned errorNumber,
                                     const char16_t** args);

extern bool ReportErrorNumberUTF8Array(JSContext* cx, IsWarning isWarning,
                                       JSErrorCallback callback, void* userRef,
                                       const unsigned errorNumber,
                                       const char** args);

extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   const char16_t** messageArgs,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorReport* reportp, va_list ap);

extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   const char** messageArgs,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorReport* reportp, va_list ap);

/*
 * For cases when we do not have an arguments array.
 */
extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorReport* reportp, va_list ap);

extern bool ExpandErrorArgumentsVA(FrontendContext* fc,
                                   JSErrorCallback callback, void* userRef,
                                   const unsigned errorNumber,
                                   const char16_t** messageArgs,
                                   ErrorArgumentsType argumentsType,
                                   JSErrorNotes::Note* notep, va_list ap);

/*
 * If there is a pending exception, print it to stderr and clear it. Otherwise
 * do nothing.
 *
 * For reporting bugs or unexpected errors in testing functions.
 */
extern void MaybePrintAndClearPendingException(JSContext* cx);

}  // namespace js

#endif /* vm_ErrorReporting_h */
