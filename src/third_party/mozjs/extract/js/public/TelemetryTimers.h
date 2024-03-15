/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_TelemetryTimers_h
#define js_TelemetryTimers_h

#include "mozilla/TimeStamp.h"

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

/** Timing information for telemetry purposes **/
struct JSTimers {
  mozilla::TimeDuration executionTime;       // Total time spent executing
  mozilla::TimeDuration delazificationTime;  // Total time spent delazifying
  mozilla::TimeDuration xdrEncodingTime;     // Total time spent XDR encoding
  mozilla::TimeDuration gcTime;              // Total time spent in GC
  mozilla::TimeDuration
      protectTime;  // Total time spent protecting JIT executable memory
  mozilla::TimeDuration
      baselineCompileTime;  // Total time spent in baseline compiler
};

extern JS_PUBLIC_API JSTimers GetJSTimers(JSContext* cx);

}  // namespace JS

#endif  // js_TelemetryTimers_h
