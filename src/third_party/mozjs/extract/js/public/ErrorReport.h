/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Error-reporting APIs.
 *
 * Despite the types and structures defined here existing in js/public,
 * significant parts of their heritage date back to distant SpiderMonkey past,
 * and they are not all universally well-thought-out as ideal,
 * intended-to-be-permanent API.  We may eventually replace this with something
 * more consistent with ECMAScript the language and less consistent with
 * '90s-era JSAPI inventions, but it's doubtful this will happen any time soon.
 */

#ifndef js_ErrorReport_h
#define js_ErrorReport_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <cstdarg>
#include <iterator>  // std::input_iterator_tag, std::iterator
#include <stdarg.h>
#include <stddef.h>  // size_t
#include <stdint.h>  // int16_t, uint16_t
#include <string.h>  // strlen

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/AllocPolicy.h"
#include "js/CharacterEncoding.h"  // JS::ConstUTF8CharsZ
#include "js/ColumnNumber.h"       // JS::ColumnNumberOneOrigin
#include "js/RootingAPI.h"         // JS::HandleObject, JS::RootedObject
#include "js/UniquePtr.h"          // js::UniquePtr
#include "js/Value.h"              // JS::Value
#include "js/Vector.h"             // js::Vector

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSString;

namespace JS {
class ExceptionStack;
}
namespace js {
class SystemAllocPolicy;

enum ErrorArgumentsType {
  ArgumentsAreUnicode,
  ArgumentsAreASCII,
  ArgumentsAreLatin1,
  ArgumentsAreUTF8
};
}  // namespace js

/**
 * Possible exception types. These types are part of a JSErrorFormatString
 * structure. They define which error to throw in case of a runtime error.
 *
 * JSEXN_WARN is used for warnings, that are not strictly errors but are handled
 * using the generalized error reporting mechanism.  (One side effect of this
 * type is to not prepend 'Error:' to warning messages.)  This value can go away
 * if we ever decide to use an entirely separate mechanism for warnings.
 */
enum JSExnType {
  JSEXN_ERR,
  JSEXN_FIRST = JSEXN_ERR,
  JSEXN_INTERNALERR,
  JSEXN_AGGREGATEERR,
  JSEXN_EVALERR,
  JSEXN_RANGEERR,
  JSEXN_REFERENCEERR,
  JSEXN_SYNTAXERR,
  JSEXN_TYPEERR,
  JSEXN_URIERR,
  JSEXN_DEBUGGEEWOULDRUN,
  JSEXN_WASMCOMPILEERROR,
  JSEXN_WASMLINKERROR,
  JSEXN_WASMRUNTIMEERROR,
  JSEXN_ERROR_LIMIT,
  JSEXN_WARN = JSEXN_ERROR_LIMIT,
  JSEXN_NOTE,
  JSEXN_LIMIT
};

struct JSErrorFormatString {
  /** The error message name in ASCII. */
  const char* name;

  /** The error format string in ASCII. */
  const char* format;

  /** The number of arguments to expand in the formatted error message. */
  uint16_t argCount;

  /** One of the JSExnType constants above. */
  int16_t exnType;
};

using JSErrorCallback =
    const JSErrorFormatString* (*)(void* userRef, const unsigned errorNumber);

/**
 * Base class that implements parts shared by JSErrorReport and
 * JSErrorNotes::Note.
 */
class JSErrorBase {
 private:
  // The (default) error message.
  // If ownsMessage_ is true, the it is freed in destructor.
  JS::ConstUTF8CharsZ message_;

 public:
  // Source file name, URL, etc., or null.
  JS::ConstUTF8CharsZ filename;

  // Unique identifier for the script source.
  unsigned sourceId;

  // Source line number (1-origin).
  uint32_t lineno;

  // Column number in line in UTF-16 code units.
  JS::ColumnNumberOneOrigin column;

  // the error number, e.g. see js/public/friend/ErrorNumbers.msg.
  unsigned errorNumber;

  // Points to JSErrorFormatString::name.
  // This string must never be freed.
  const char* errorMessageName;

 private:
  bool ownsMessage_ : 1;

 public:
  JSErrorBase()
      : filename(nullptr),
        sourceId(0),
        lineno(0),
        errorNumber(0),
        errorMessageName(nullptr),
        ownsMessage_(false) {}
  JSErrorBase(JSErrorBase&& other) noexcept
      : message_(other.message_),
        filename(other.filename),
        sourceId(other.sourceId),
        lineno(other.lineno),
        column(other.column),
        errorNumber(other.errorNumber),
        errorMessageName(other.errorMessageName),
        ownsMessage_(other.ownsMessage_) {
    if (ownsMessage_) {
      other.ownsMessage_ = false;
    }
  }

  ~JSErrorBase() { freeMessage(); }

 public:
  const JS::ConstUTF8CharsZ message() const { return message_; }

  void initOwnedMessage(const char* messageArg) {
    initBorrowedMessage(messageArg);
    ownsMessage_ = true;
  }
  void initBorrowedMessage(const char* messageArg) {
    MOZ_ASSERT(!message_);
    message_ = JS::ConstUTF8CharsZ(messageArg, strlen(messageArg));
  }

  JSString* newMessageString(JSContext* cx);

 private:
  void freeMessage();
};

/**
 * Notes associated with JSErrorReport.
 */
class JSErrorNotes {
 public:
  class Note final : public JSErrorBase {};

 private:
  // Stores pointers to each note.
  js::Vector<js::UniquePtr<Note>, 1, js::SystemAllocPolicy> notes_;

  bool addNoteVA(js::FrontendContext* fc, const char* filename,
                 unsigned sourceId, uint32_t lineno,
                 JS::ColumnNumberOneOrigin column,
                 JSErrorCallback errorCallback, void* userRef,
                 const unsigned errorNumber,
                 js::ErrorArgumentsType argumentsType, va_list ap);

 public:
  JSErrorNotes();
  ~JSErrorNotes();

  // Add a note to the given position.
  bool addNoteASCII(JSContext* cx, const char* filename, unsigned sourceId,
                    uint32_t lineno, JS::ColumnNumberOneOrigin column,
                    JSErrorCallback errorCallback, void* userRef,
                    const unsigned errorNumber, ...);
  bool addNoteASCII(js::FrontendContext* fc, const char* filename,
                    unsigned sourceId, uint32_t lineno,
                    JS::ColumnNumberOneOrigin column,
                    JSErrorCallback errorCallback, void* userRef,
                    const unsigned errorNumber, ...);
  bool addNoteLatin1(JSContext* cx, const char* filename, unsigned sourceId,
                     uint32_t lineno, JS::ColumnNumberOneOrigin column,
                     JSErrorCallback errorCallback, void* userRef,
                     const unsigned errorNumber, ...);
  bool addNoteLatin1(js::FrontendContext* fc, const char* filename,
                     unsigned sourceId, uint32_t lineno,
                     JS::ColumnNumberOneOrigin column,
                     JSErrorCallback errorCallback, void* userRef,
                     const unsigned errorNumber, ...);
  bool addNoteUTF8(JSContext* cx, const char* filename, unsigned sourceId,
                   uint32_t lineno, JS::ColumnNumberOneOrigin column,
                   JSErrorCallback errorCallback, void* userRef,
                   const unsigned errorNumber, ...);
  bool addNoteUTF8(js::FrontendContext* fc, const char* filename,
                   unsigned sourceId, uint32_t lineno,
                   JS::ColumnNumberOneOrigin column,
                   JSErrorCallback errorCallback, void* userRef,
                   const unsigned errorNumber, ...);

  JS_PUBLIC_API size_t length();

  // Create a deep copy of notes.
  js::UniquePtr<JSErrorNotes> copy(JSContext* cx);

  class iterator final {
   private:
    js::UniquePtr<Note>* note_;

   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = js::UniquePtr<Note>;
    using difference_type = ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;

    explicit iterator(js::UniquePtr<Note>* note = nullptr) : note_(note) {}

    bool operator==(iterator other) const { return note_ == other.note_; }
    bool operator!=(iterator other) const { return !(*this == other); }
    iterator& operator++() {
      note_++;
      return *this;
    }
    reference operator*() { return *note_; }
  };

  JS_PUBLIC_API iterator begin();
  JS_PUBLIC_API iterator end();
};

/**
 * Describes a single error or warning that occurs in the execution of script.
 */
class JSErrorReport : public JSErrorBase {
 private:
  // Offending source line without final '\n'.
  // If ownsLinebuf_ is true, the buffer is freed in destructor.
  const char16_t* linebuf_;

  // Number of chars in linebuf_. Does not include trailing '\0'.
  size_t linebufLength_;

  // The 0-based offset of error token in linebuf_.
  size_t tokenOffset_;

 public:
  // Associated notes, or nullptr if there's no note.
  js::UniquePtr<JSErrorNotes> notes;

  // One of the JSExnType constants.
  int16_t exnType;

  // See the comment in TransitiveCompileOptions.
  bool isMuted : 1;

  // This error report is actually a warning.
  bool isWarning_ : 1;

 private:
  bool ownsLinebuf_ : 1;

 public:
  JSErrorReport()
      : linebuf_(nullptr),
        linebufLength_(0),
        tokenOffset_(0),
        notes(nullptr),
        exnType(0),
        isMuted(false),
        isWarning_(false),
        ownsLinebuf_(false) {}
  JSErrorReport(JSErrorReport&& other) noexcept
      : JSErrorBase(std::move(other)),
        linebuf_(other.linebuf_),
        linebufLength_(other.linebufLength_),
        tokenOffset_(other.tokenOffset_),
        notes(std::move(other.notes)),
        exnType(other.exnType),
        isMuted(other.isMuted),
        isWarning_(other.isWarning_),
        ownsLinebuf_(other.ownsLinebuf_) {
    if (ownsLinebuf_) {
      other.ownsLinebuf_ = false;
    }
  }

  ~JSErrorReport() { freeLinebuf(); }

 public:
  const char16_t* linebuf() const { return linebuf_; }
  size_t linebufLength() const { return linebufLength_; }
  size_t tokenOffset() const { return tokenOffset_; }
  void initOwnedLinebuf(const char16_t* linebufArg, size_t linebufLengthArg,
                        size_t tokenOffsetArg) {
    initBorrowedLinebuf(linebufArg, linebufLengthArg, tokenOffsetArg);
    ownsLinebuf_ = true;
  }
  void initBorrowedLinebuf(const char16_t* linebufArg, size_t linebufLengthArg,
                           size_t tokenOffsetArg);

  bool isWarning() const { return isWarning_; }

 private:
  void freeLinebuf();
};

namespace JS {

struct MOZ_STACK_CLASS JS_PUBLIC_API ErrorReportBuilder {
  explicit ErrorReportBuilder(JSContext* cx);
  ~ErrorReportBuilder();

  enum SniffingBehavior { WithSideEffects, NoSideEffects };

  /**
   * Generate a JSErrorReport from the provided thrown value.
   *
   * If the value is a (possibly wrapped) Error object, the JSErrorReport will
   * be exactly initialized from the Error object's information, without
   * observable side effects. (The Error object's JSErrorReport is reused, if
   * it has one.)
   *
   * Otherwise various attempts are made to derive JSErrorReport information
   * from |exnStack| and from the current execution state.  This process is
   * *definitely* inconsistent with any standard, and particulars of the
   * behavior implemented here generally shouldn't be relied upon.
   *
   * If the value of |sniffingBehavior| is |WithSideEffects|, some of these
   * attempts *may* invoke user-configurable behavior when the exception is an
   * object: converting it to a string, detecting and getting its properties,
   * accessing its prototype chain, and others are possible.  Users *must*
   * tolerate |ErrorReportBuilder::init| potentially having arbitrary effects.
   * Any exceptions thrown by these operations will be caught and silently
   * ignored, and "default" values will be substituted into the JSErrorReport.
   *
   * But if the value of |sniffingBehavior| is |NoSideEffects|, these attempts
   * *will not* invoke any observable side effects.  The JSErrorReport will
   * simply contain fewer, less precise details.
   *
   * Unlike some functions involved in error handling, this function adheres
   * to the usual JSAPI return value error behavior.
   */
  bool init(JSContext* cx, const JS::ExceptionStack& exnStack,
            SniffingBehavior sniffingBehavior);

  JSErrorReport* report() const { return reportp; }

  const JS::ConstUTF8CharsZ toStringResult() const { return toStringResult_; }

 private:
  // More or less an equivalent of JS_ReportErrorNumber/js::ReportErrorNumberVA
  // but fills in an ErrorReport instead of reporting it.  Uses varargs to
  // make it simpler to call js::ExpandErrorArgumentsVA.
  //
  // Returns false if we fail to actually populate the ErrorReport
  // for some reason (probably out of memory).
  bool populateUncaughtExceptionReportUTF8(JSContext* cx,
                                           JS::HandleObject stack, ...);
  bool populateUncaughtExceptionReportUTF8VA(JSContext* cx,
                                             JS::HandleObject stack,
                                             va_list ap);

  // Reports exceptions from add-on scopes to telemetry.
  void ReportAddonExceptionToTelemetry(JSContext* cx);

  // We may have a provided JSErrorReport, so need a way to represent that.
  JSErrorReport* reportp;

  // Or we may need to synthesize a JSErrorReport one of our own.
  JSErrorReport ownedReport;

  // Root our exception value to keep a possibly borrowed |reportp| alive.
  JS::RootedObject exnObject;

  // And for our filename.
  JS::UniqueChars filename;

  // We may have a result of error.toString().
  // FIXME: We should not call error.toString(), since it could have side
  //        effect (see bug 633623).
  JS::ConstUTF8CharsZ toStringResult_;
  JS::UniqueChars toStringResultBytesStorage;
};

// Writes a full report to a file descriptor. Does nothing for JSErrorReports
// which are warnings, unless reportWarnings is set.
extern JS_PUBLIC_API void PrintError(FILE* file, JSErrorReport* report,
                                     bool reportWarnings);

extern JS_PUBLIC_API void PrintError(FILE* file,
                                     const JS::ErrorReportBuilder& builder,
                                     bool reportWarnings);

}  // namespace JS

/*
 * There are four encoding variants for the error reporting API:
 *   UTF-8
 *     JSAPI's default encoding for error handling.  Use this when the encoding
 *     of the error message, format string, and arguments is UTF-8.
 *   ASCII
 *     Equivalent to UTF-8, but also asserts that the error message, format
 *     string, and arguments are all ASCII.  Because ASCII is a subset of UTF-8,
 *     any use of this encoding variant *could* be replaced with use of the
 *     UTF-8 variant.  This variant exists solely to double-check the
 *     developer's assumption that all these strings truly are ASCII, given that
 *     UTF-8 and ASCII strings regrettably have the same C++ type.
 *   UC = UTF-16
 *     Use this when arguments are UTF-16.  The format string must be UTF-8.
 *   Latin1 (planned to be removed)
 *     In this variant, all strings are interpreted byte-for-byte as the
 *     corresponding Unicode codepoint.  This encoding may *safely* be used on
 *     any null-terminated string, regardless of its encoding.  (You shouldn't
 *     *actually* be uncertain, but in the real world, a string's encoding -- if
 *     promised at all -- may be more...aspirational...than reality.)  This
 *     encoding variant will eventually be removed -- work to convert your uses
 *     to UTF-8 as you're able.
 */

namespace JS {
const uint16_t MaxNumErrorArguments = 10;
};

/**
 * Report an exception represented by the sprintf-like conversion of format
 * and its arguments.
 */
extern JS_PUBLIC_API void JS_ReportErrorASCII(JSContext* cx, const char* format,
                                              ...) MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API void JS_ReportErrorLatin1(JSContext* cx,
                                               const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API void JS_ReportErrorUTF8(JSContext* cx, const char* format,
                                             ...) MOZ_FORMAT_PRINTF(2, 3);

/*
 * Use an errorNumber to retrieve the format string, args are char*
 */
extern JS_PUBLIC_API void JS_ReportErrorNumberASCII(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, ...);

extern JS_PUBLIC_API void JS_ReportErrorNumberASCIIVA(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, va_list ap);

extern JS_PUBLIC_API void JS_ReportErrorNumberLatin1(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, ...);

#ifdef va_start
extern JS_PUBLIC_API void JS_ReportErrorNumberLatin1VA(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, va_list ap);
#endif

extern JS_PUBLIC_API void JS_ReportErrorNumberUTF8(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, ...);

#ifdef va_start
extern JS_PUBLIC_API void JS_ReportErrorNumberUTF8VA(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, va_list ap);
#endif

/*
 * args is null-terminated.  That is, a null char* means there are no
 * more args.  The number of args must match the number expected for
 * errorNumber for the given JSErrorCallback.
 */
extern JS_PUBLIC_API void JS_ReportErrorNumberUTF8Array(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, const char** args);

/*
 * Use an errorNumber to retrieve the format string, args are char16_t*
 */
extern JS_PUBLIC_API void JS_ReportErrorNumberUC(JSContext* cx,
                                                 JSErrorCallback errorCallback,
                                                 void* userRef,
                                                 const unsigned errorNumber,
                                                 ...);

extern JS_PUBLIC_API void JS_ReportErrorNumberUCArray(
    JSContext* cx, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, const char16_t** args);

/**
 * Complain when out of memory.
 */
extern MOZ_COLD JS_PUBLIC_API void JS_ReportOutOfMemory(JSContext* cx);

extern JS_PUBLIC_API bool JS_ExpandErrorArgumentsASCII(
    JSContext* cx, JSErrorCallback errorCallback, const unsigned errorNumber,
    JSErrorReport* reportp, ...);

/**
 * Complain when an allocation size overflows the maximum supported limit.
 */
extern JS_PUBLIC_API void JS_ReportAllocationOverflow(JSContext* cx);

namespace JS {

extern JS_PUBLIC_API bool CreateError(
    JSContext* cx, JSExnType type, HandleObject stack, HandleString fileName,
    uint32_t lineNumber, JS::ColumnNumberOneOrigin column,
    JSErrorReport* report, HandleString message,
    Handle<mozilla::Maybe<Value>> cause, MutableHandleValue rval);

} /* namespace JS */

#endif /* js_ErrorReport_h */
