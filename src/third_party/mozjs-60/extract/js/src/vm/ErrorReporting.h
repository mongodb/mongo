/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorReporting_h
#define vm_ErrorReporting_h

#include "mozilla/Move.h"

#include <stdarg.h>

#include "jsapi.h" // for JSErrorNotes, JSErrorReport

#include "js/UniquePtr.h" // for UniquePtr
#include "js/Utility.h" // for UniqueTwoByteChars

namespace js {

/**
 * Metadata for a compilation error (or warning) at a particular offset, or at
 * no offset (i.e. with respect to a script overall).
 */
struct ErrorMetadata
{
    // The file/URL where the error occurred.
    const char* filename;

    // The line and column numbers where the error occurred.  If the error
    // is with respect to the entire script and not with respect to a
    // particular location, these will both be zero.
    uint32_t lineNumber;
    uint32_t columnNumber;

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

class CompileError : public JSErrorReport
{
  public:
    void throwError(JSContext* cx);
};

/** Send a JSErrorReport to the warningReporter callback. */
extern void
CallWarningReporter(JSContext* cx, JSErrorReport* report);

/**
 * Report a compile error during script processing prior to execution of the
 * script.
 */
extern void
ReportCompileError(JSContext* cx, ErrorMetadata&& metadata, UniquePtr<JSErrorNotes> notes,
                   unsigned flags, unsigned errorNumber, va_list args);

/**
 * Report a compile warning during script processing prior to execution of the
 * script.  Returns true if the warning was successfully reported, false if an
 * error occurred.
 *
 * This function DOES NOT respect an existing werror option.  If the caller
 * wishes such option to be respected, it must do so itself.
 */
extern MOZ_MUST_USE bool
ReportCompileWarning(JSContext* cx, ErrorMetadata&& metadata, UniquePtr<JSErrorNotes> notes,
                     unsigned flags, unsigned errorNumber, va_list args);

/**
 * Report the given error Value to the given global.  The JSContext is not
 * assumed to be in any particular compartment, but the global and error are
 * expected to be same-compartment.
 */
extern void
ReportErrorToGlobal(JSContext* cx, JS::HandleObject global, JS::HandleValue error);

} // namespace js

#endif /* vm_ErrorReporting_h */
