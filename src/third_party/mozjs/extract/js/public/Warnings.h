/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Functionality for issuing and handling warnings.
 *
 * Warnings are situations that aren't inherently full-blown errors (and perhaps
 * for spec compliance *can't* be), but that may represent dubious programming
 * practice that embeddings may wish to know about.
 *
 * SpiderMonkey recognizes an unspecified set of syntactic patterns and runtime
 * behaviors as triggering a warning.  Embeddings may also recognize and report
 * additional warnings.
 */

#ifndef js_Warnings_h
#define js_Warnings_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_FORMAT_PRINTF, MOZ_RAII

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JSErrorReport;

namespace JS {

/**
 * Report a warning represented by the sprintf-like conversion of ASCII format
 * filled from trailing ASCII arguments.
 *
 * Return true iff the warning was successfully reported without reporting an
 * error (or being upgraded into one).
 */
extern JS_PUBLIC_API bool WarnASCII(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

/**
 * Report a warning represented by the sprintf-like conversion of Latin-1 format
 * filled from trailing Latin-1 arguments.
 *
 * Return true iff the warning was successfully reported without reporting an
 * error (or being upgraded into one).
 */
extern JS_PUBLIC_API bool WarnLatin1(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

/**
 * Report a warning represented by the sprintf-like conversion of UTF-8 format
 * filled from trailing UTF-8 arguments.
 *
 * Return true iff the warning was successfully reported without reporting an
 * error (or being upgraded into one).
 */
extern JS_PUBLIC_API bool WarnUTF8(JSContext* cx, const char* format, ...)
    MOZ_FORMAT_PRINTF(2, 3);

using WarningReporter = void (*)(JSContext* cx, JSErrorReport* report);

extern JS_PUBLIC_API WarningReporter GetWarningReporter(JSContext* cx);

extern JS_PUBLIC_API WarningReporter
SetWarningReporter(JSContext* cx, WarningReporter reporter);

/**
 * A simple RAII class that clears the registered warning reporter on
 * construction and restores it on destruction.
 *
 * A fresh warning reporter *may* be set while an instance of this class is
 * live, but it must be unset in LIFO fashion by the time that instance is
 * destroyed.
 */
class MOZ_RAII JS_PUBLIC_API AutoSuppressWarningReporter {
  JSContext* context_;
  WarningReporter prevReporter_;

 public:
  explicit AutoSuppressWarningReporter(JSContext* cx) : context_(cx) {
    prevReporter_ = SetWarningReporter(context_, nullptr);
  }

  ~AutoSuppressWarningReporter() {
#ifdef DEBUG
    WarningReporter reporter =
#endif
        SetWarningReporter(context_, prevReporter_);
    MOZ_ASSERT(reporter == nullptr, "Unexpected WarningReporter active");
    SetWarningReporter(context_, prevReporter_);
  }
};

}  // namespace JS

#endif  // js_Warnings_h
