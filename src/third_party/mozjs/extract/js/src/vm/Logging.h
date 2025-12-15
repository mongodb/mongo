/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// An experimental logging interface for connecting the JS Engine to some
// (*cough*Gecko*cough*) consumer.

#ifndef _js_vm_Logging_h_
#define _js_vm_Logging_h_

#include "mozilla/Assertions.h"
#include "mozilla/LoggingCore.h"

#include "jit/JitSpewer.h"
#include "js/experimental/LoggingInterface.h"

struct JSContext;

namespace js {

using mozilla::LogLevel;

// [SMDOC] js::LogModule
//
// js::LogModule is the underlying type used for JS_LOG.
//
// To support declaring these statically, while simultaneously supporting the
// initialization of the interfaces via a callback, each instance of LogModule
// is registered in a module register (logModuleRegistry), which updates
// the module interface for each log module.
//
// Log modules are declared below using a Macro to support the metaprogramming
// required around their storage an initialization.
class LogModule {
 public:
  explicit constexpr LogModule(const char* name) : name(name) {
    MOZ_ASSERT(name);
  }

  // Return true iff we should log a message at this level.
  inline bool shouldLog(mozilla::LogLevel level) const {
    if (!isSetup()) {
      return false;
    }

    return *levelPtr >= level;
  }

  // Initialize all LogModules to speak with the provided interface.
  [[nodiscard]] static bool initializeAll(const JS::LoggingInterface iface);

 public:
  // Public as it's used by the macro below, and we don't need a
  // forwarding interface.
  mutable JS::LoggingInterface interface{};

  // Opaque logger obtained via the interface; also public for macro useage.
  mutable JS::OpaqueLogger logger{};

  // Name of this logger
  const char* name{};

 private:
  // Is this logger ready to be used.
  inline bool isSetup() const { return interface.isComplete() && logger; }

  // Initialize this Log module
  bool initialize(const JS::LoggingInterface iface) const {
    // Grab a local copy of the iface.
    interface = iface;
    MOZ_ASSERT(iface.isComplete());
    logger = iface.getLoggerByName(name);
    if (!logger) {
      return false;
    }

    levelPtr = &iface.getLevelRef(logger);
    return true;
  }

  // Used to fast-path check if we should log.
  mutable mozilla::AtomicLogLevel* levelPtr{};
};

#define FOR_EACH_JS_LOG_MODULE(_)                                            \
  _(debug)                /* A predefined log module for casual debugging */ \
  _(wasmPerf)             /* Wasm performance statistics */                  \
  _(wasmApi)              /* Wasm JS-API tracing */                          \
  _(fuseInvalidation)     /* Invalidation triggered by a fuse  */            \
  _(thenable)             /* Thenable on standard proto*/                    \
  _(startup)              /* engine startup logging */                       \
  _(teleporting)          /* Shape Teleporting */                            \
  _(selfHosted)           /* self-hosted script logging */                   \
  JITSPEW_CHANNEL_LIST(_) /* A module for each JitSpew channel. */

// Declare Log modules
#define DECLARE_MODULE(X) inline constexpr LogModule X##Module(#X);

FOR_EACH_JS_LOG_MODULE(DECLARE_MODULE);

#undef DECLARE_MODULE

// By default JS_LOGGING is enabled; but if we would like this become
// conditional this file-internal macro can be used to accomplish that.
#define JS_LOGGING 1

// The core logging macro for the JS Engine.
#ifdef JS_LOGGING
#  define JS_SHOULD_LOG(name, log_level) \
    name##Module.shouldLog(LogLevel::log_level)

#  define JS_LOG(name, log_level, ...)                                     \
    do {                                                                   \
      if (name##Module.shouldLog(LogLevel::log_level)) {                   \
        name##Module.interface.logPrint(name##Module.logger,               \
                                        LogLevel::log_level, __VA_ARGS__); \
      }                                                                    \
    } while (0);
#  define JS_LOG_FMT(name, log_level, fmt, ...)                             \
    do {                                                                    \
      if (name##Module.shouldLog(LogLevel::log_level)) {                    \
        name##Module.interface.logPrintFmt(name##Module.logger,             \
                                           LogLevel::log_level,             \
                                           FMT_STRING(fmt), ##__VA_ARGS__); \
      }                                                                     \
    } while (0);
#else
#  define JS_LOG(module, log_level, ...)
#  define JS_LOG_FMT(module, log_level, fmt, ...)
#endif

#undef JS_LOGGING

}  // namespace js

#endif /* _js_vm_Logging_h_ */
