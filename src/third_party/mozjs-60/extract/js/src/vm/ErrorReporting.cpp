/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ErrorReporting.h"

#include "mozilla/Move.h"

#include <stdarg.h>

#include "jsexn.h"
#include "jsfriendapi.h"

#include "vm/JSContext.h"

#include "vm/JSContext-inl.h"

using mozilla::Move;

using JS::HandleObject;
using JS::HandleValue;
using JS::UniqueTwoByteChars;

void
js::CallWarningReporter(JSContext* cx, JSErrorReport* reportp)
{
    MOZ_ASSERT(reportp);
    MOZ_ASSERT(JSREPORT_IS_WARNING(reportp->flags));

    if (JS::WarningReporter warningReporter = cx->runtime()->warningReporter)
        warningReporter(cx, reportp);
}

void
js::CompileError::throwError(JSContext* cx)
{
    if (JSREPORT_IS_WARNING(flags)) {
        CallWarningReporter(cx, this);
        return;
    }

    // If there's a runtime exception type associated with this error
    // number, set that as the pending exception.  For errors occuring at
    // compile time, this is very likely to be a JSEXN_SYNTAXERR.
    //
    // If an exception is thrown but not caught, the JSREPORT_EXCEPTION
    // flag will be set in report.flags.  Proper behavior for an error
    // reporter is to ignore a report with this flag for all but top-level
    // compilation errors.  The exception will remain pending, and so long
    // as the non-top-level "load", "eval", or "compile" native function
    // returns false, the top-level reporter will eventually receive the
    // uncaught exception report.
    ErrorToException(cx, this, nullptr, nullptr);
}

bool
js::ReportCompileWarning(JSContext* cx, ErrorMetadata&& metadata, UniquePtr<JSErrorNotes> notes,
                         unsigned flags, unsigned errorNumber, va_list args)
{
    // On the active thread, report the error immediately. When compiling off
    // thread, save the error so that the thread finishing the parse can report
    // it later.
    CompileError tempErr;
    CompileError* err = &tempErr;
    if (cx->helperThread() && !cx->addPendingCompileError(&err))
        return false;

    err->notes = Move(notes);
    err->flags = flags;
    err->errorNumber = errorNumber;

    err->filename = metadata.filename;
    err->lineno = metadata.lineNumber;
    err->column = metadata.columnNumber;
    err->isMuted = metadata.isMuted;

    if (UniqueTwoByteChars lineOfContext = Move(metadata.lineOfContext))
        err->initOwnedLinebuf(lineOfContext.release(), metadata.lineLength, metadata.tokenOffset);

    if (!ExpandErrorArgumentsVA(cx, GetErrorMessage, nullptr, errorNumber,
                                nullptr, ArgumentsAreLatin1, err, args))
    {
        return false;
    }

    if (!cx->helperThread())
        err->throwError(cx);

    return true;
}

void
js::ReportCompileError(JSContext* cx, ErrorMetadata&& metadata, UniquePtr<JSErrorNotes> notes,
                       unsigned flags, unsigned errorNumber, va_list args)
{
    // On the active thread, report the error immediately. When compiling off
    // thread, save the error so that the thread finishing the parse can report
    // it later.
    CompileError tempErr;
    CompileError* err = &tempErr;
    if (cx->helperThread() && !cx->addPendingCompileError(&err))
        return;

    err->notes = Move(notes);
    err->flags = flags;
    err->errorNumber = errorNumber;

    err->filename = metadata.filename;
    err->lineno = metadata.lineNumber;
    err->column = metadata.columnNumber;
    err->isMuted = metadata.isMuted;

    if (UniqueTwoByteChars lineOfContext = Move(metadata.lineOfContext))
        err->initOwnedLinebuf(lineOfContext.release(), metadata.lineLength, metadata.tokenOffset);

    if (!ExpandErrorArgumentsVA(cx, GetErrorMessage, nullptr, errorNumber,
                                nullptr, ArgumentsAreLatin1, err, args))
    {
        return;
    }

    if (!cx->helperThread())
        err->throwError(cx);
}

namespace {

class MOZ_STACK_CLASS ReportExceptionClosure
  : public js::ScriptEnvironmentPreparer::Closure
{
  public:
    explicit ReportExceptionClosure(HandleValue& exn)
      : exn_(exn)
    {
    }

    bool operator()(JSContext* cx) override
    {
        cx->setPendingException(exn_);
        return false;
    }

  private:
    HandleValue& exn_;
};

} // anonymous namespace

void
js::ReportErrorToGlobal(JSContext* cx, HandleObject global, HandleValue error)
{
    MOZ_ASSERT(!cx->isExceptionPending());
#ifdef DEBUG
    // No assertSameCompartment version that doesn't take JSContext...
    if (error.isObject()) {
        AssertSameCompartment(global, &error.toObject());
    }
#endif // DEBUG
    ReportExceptionClosure report(error);
    PrepareScriptEnvironmentAndInvoke(cx, global, report);
}
