/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Warnings.h"
#include "vm/Warnings.h"

#include <stdarg.h>  // va_{list,start,end}

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Context.h"               // js::AssertHeapIsIdle
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage
#include "vm/ErrorReporting.h"        // IsWarning
#include "vm/JSContext.h"  // js::ArgumentsAre{ASCII,Latin1,UTF8}, js::ReportError{Number}VA

using js::ArgumentsAreASCII;
using js::ArgumentsAreLatin1;
using js::ArgumentsAreUTF8;
using js::AssertHeapIsIdle;
using js::GetErrorMessage;
using js::IsWarning;
using js::ReportErrorVA;

JS_PUBLIC_API bool JS::WarnASCII(JSContext* cx, const char* format, ...) {
  va_list ap;
  bool ok;

  AssertHeapIsIdle();
  va_start(ap, format);
  ok = ReportErrorVA(cx, IsWarning::Yes, format, ArgumentsAreASCII, ap);
  va_end(ap);
  return ok;
}

JS_PUBLIC_API bool JS::WarnLatin1(JSContext* cx, const char* format, ...) {
  va_list ap;
  bool ok;

  AssertHeapIsIdle();
  va_start(ap, format);
  ok = ReportErrorVA(cx, IsWarning::Yes, format, ArgumentsAreLatin1, ap);
  va_end(ap);
  return ok;
}

JS_PUBLIC_API bool JS::WarnUTF8(JSContext* cx, const char* format, ...) {
  va_list ap;
  bool ok;

  AssertHeapIsIdle();
  va_start(ap, format);
  ok = ReportErrorVA(cx, IsWarning::Yes, format, ArgumentsAreUTF8, ap);
  va_end(ap);
  return ok;
}

JS_PUBLIC_API JS::WarningReporter JS::GetWarningReporter(JSContext* cx) {
  return cx->runtime()->warningReporter;
}

JS_PUBLIC_API JS::WarningReporter JS::SetWarningReporter(
    JSContext* cx, WarningReporter reporter) {
  WarningReporter older = cx->runtime()->warningReporter;
  cx->runtime()->warningReporter = reporter;
  return older;
}

bool js::WarnNumberASCII(JSContext* cx, const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  bool ok = ReportErrorNumberVA(cx, IsWarning::Yes, GetErrorMessage, nullptr,
                                errorNumber, ArgumentsAreASCII, ap);
  va_end(ap);
  return ok;
}

bool js::WarnNumberLatin1(JSContext* cx, const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  bool ok = ReportErrorNumberVA(cx, IsWarning::Yes, GetErrorMessage, nullptr,
                                errorNumber, ArgumentsAreLatin1, ap);
  va_end(ap);
  return ok;
}

bool js::WarnNumberUTF8(JSContext* cx, const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  bool ok = ReportErrorNumberVA(cx, IsWarning::Yes, GetErrorMessage, nullptr,
                                errorNumber, ArgumentsAreUTF8, ap);
  va_end(ap);
  return ok;
}

bool js::WarnNumberUC(JSContext* cx, const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  bool ok = ReportErrorNumberVA(cx, IsWarning::Yes, GetErrorMessage, nullptr,
                                errorNumber, ArgumentsAreUnicode, ap);
  va_end(ap);
  return ok;
}
