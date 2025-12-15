/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// An experimental logging interface for connecting the JS Engine to some
// (*cough*Gecko*cough*) consumer.

#ifndef _js_experimental_LoggingInterface_h_
#define _js_experimental_LoggingInterface_h_

#include "mozilla/LoggingCore.h"

#include "jstypes.h"
#include "fmt/format.h"
#include "js/GCAPI.h"

struct JSContext;

namespace JS {

// An Opaque pointer to a LoggerType. It must be possible for this logger type
// to conform to the interface below, using the LogLevel type exported in
// mozglue LoggingCore.h
//
// There are some requirements that we cannot express through the type-system:
//
// - A Logger must outlive any caller. This is an obvious statement, but in the
//   context of the JS engine means that a logger must live until after the JS
//   engine is shutdown.
// - A logger cannot move. The logging interfaces assumes 1) That an
//   OpaqueLogger will remain a valid handle to a logger for the entire duration
//   of an initialize JS library, and 2) We are able to cache a reference to the
//   log level of a particular logger (see getLevelRef below).
using OpaqueLogger = void*;

// [SMDOC] Logging Interface
//
// The logging interface contains a set of function pointers which explain how
// to talk to an embedder provided logging system.
//
// The design of the JS Consumer of this relies heavily on these to be freely
// copyable.
struct LoggingInterface {
  // Acquire a new logger for a given name.
  //
  // This interface has no way of indicating backwards that a logger is no
  // longer needed, and as such this pointer needs to be kept alive by the
  // embedding for the lifetime of the JS engine.
  OpaqueLogger (*getLoggerByName)(const char* loggerName) = nullptr;

  // Print a message to a particular logger with a particular level and
  // format.
  void (*logPrintVA)(const OpaqueLogger aModule, mozilla::LogLevel aLevel,
                     const char* aFmt, va_list ap)
      MOZ_FORMAT_PRINTF(3, 0) = nullptr;

  void (*logPrintFMT)(const OpaqueLogger aModule, mozilla::LogLevel aLevel,
                      fmt::string_view, fmt::format_args);

  // Return a reference to the provided OpaqueLogger's level ref; Implementation
  // wise this can be a small violation of encapsulation but is intended to help
  // ensure that we can build lightweight logging without egregious costs to
  // simply check even if a mesage will the writen
  mozilla::AtomicLogLevel& (*getLevelRef)(OpaqueLogger) = nullptr;

  // Wrapper function for calling va-version
  void logPrint(const OpaqueLogger aModule, mozilla::LogLevel aLevel,
                const char* aFmt, ...) MOZ_FORMAT_PRINTF(4, 5) {
    JS::AutoSuppressGCAnalysis suppress;
    va_list ap;
    va_start(ap, aFmt);
    this->logPrintVA(aModule, aLevel, aFmt, ap);
    va_end(ap);
  }

  template <typename... T>
  void logPrintFmt(const OpaqueLogger aModule, mozilla::LogLevel aLevel,
                   fmt::format_string<T...> aFmt, T&&... aArgs) {
    JS::AutoSuppressGCAnalysis suppress;
    this->logPrintFMT(aModule, aLevel, aFmt, fmt::make_format_args(aArgs...));
  }

  // Used to ensure that before we use an interface, it's successfully been
  // completely filled in.
  bool isComplete() const {
    return getLoggerByName && logPrintVA && getLevelRef;
  }
};

// Install the logging interface. This will also install the interface into
// any JS loggers
extern JS_PUBLIC_API bool SetLoggingInterface(LoggingInterface& iface);

}  // namespace JS

#endif /* _js_experimental_LoggingInterface_h_ */
