/* vim: set shiftwidth=2 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Shared logging infrastructure across different binaries.

#ifndef _mozilla_LoggingCore_h
#define _mozilla_LoggingCore_h

#include "mozilla/Atomics.h"
#include "mozilla/Types.h"

namespace mozilla {
// While not a 100% mapping to PR_LOG's numeric values, mozilla::LogLevel does
// maintain a direct mapping for the Disabled, Debug and Verbose levels.
//
// Mappings of LogLevel to PR_LOG's numeric values:
//
//   +---------+------------------+-----------------+
//   | Numeric | NSPR Logging     | Mozilla Logging |
//   +---------+------------------+-----------------+
//   |       0 | PR_LOG_NONE      | Disabled        |
//   |       1 | PR_LOG_ALWAYS    | Error           |
//   |       2 | PR_LOG_ERROR     | Warning         |
//   |       3 | PR_LOG_WARNING   | Info            |
//   |       4 | PR_LOG_DEBUG     | Debug           |
//   |       5 | PR_LOG_DEBUG + 1 | Verbose         |
//   +---------+------------------+-----------------+
//
enum class LogLevel {
  Disabled = 0,
  Error,
  Warning,
  Info,
  Debug,
  Verbose,
};

/**
 * Safely converts an integer into a valid LogLevel.
 */
MFBT_API LogLevel ToLogLevel(int32_t aLevel);

using AtomicLogLevel = Atomic<LogLevel, Relaxed>;

}  // namespace mozilla

#endif /* _mozilla_LoggingCore_h */
