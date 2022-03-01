/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Shadow definition of |JS::Zone| innards.  Do not use this directly! */

#ifndef js_shadow_Zone_h
#define js_shadow_Zone_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint8_t, uint32_t

#include "jspubtd.h"  // js::CurrentThreadCanAccessRuntime

struct JS_PUBLIC_API JSRuntime;
class JS_PUBLIC_API JSTracer;

namespace JS {

namespace shadow {

struct Zone {
  enum GCState : uint8_t {
    NoGC,
    Prepare,
    MarkBlackOnly,
    MarkBlackAndGray,
    Sweep,
    Finished,
    Compact
  };

  enum Kind : uint8_t { NormalZone, AtomsZone, SelfHostingZone, SystemZone };

 protected:
  JSRuntime* const runtime_;
  JSTracer* const barrierTracer_;  // A pointer to the JSRuntime's |gcMarker|.
  uint32_t needsIncrementalBarrier_ = 0;
  GCState gcState_ = NoGC;
  const Kind kind_;

  Zone(JSRuntime* runtime, JSTracer* barrierTracerArg, Kind kind)
      : runtime_(runtime), barrierTracer_(barrierTracerArg), kind_(kind) {}

 public:
  bool needsIncrementalBarrier() const { return needsIncrementalBarrier_; }

  JSTracer* barrierTracer() {
    MOZ_ASSERT(needsIncrementalBarrier_);
    MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime_));
    return barrierTracer_;
  }

  JSRuntime* runtimeFromMainThread() const {
    MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(runtime_));
    return runtime_;
  }

  // Note: Unrestricted access to the zone's runtime from an arbitrary
  // thread can easily lead to races. Use this method very carefully.
  JSRuntime* runtimeFromAnyThread() const { return runtime_; }

  GCState gcState() const { return gcState_; }
  bool wasGCStarted() const { return gcState_ != NoGC; }
  bool isGCPreparing() const { return gcState_ == Prepare; }
  bool isGCMarkingBlackOnly() const { return gcState_ == MarkBlackOnly; }
  bool isGCMarkingBlackAndGray() const { return gcState_ == MarkBlackAndGray; }
  bool isGCSweeping() const { return gcState_ == Sweep; }
  bool isGCFinished() const { return gcState_ == Finished; }
  bool isGCCompacting() const { return gcState_ == Compact; }
  bool isGCMarking() const {
    return isGCMarkingBlackOnly() || isGCMarkingBlackAndGray();
  }
  bool isGCMarkingOrSweeping() const {
    return gcState_ >= MarkBlackOnly && gcState_ <= Sweep;
  }
  bool isGCSweepingOrCompacting() const {
    return gcState_ == Sweep || gcState_ == Compact;
  }

  bool isAtomsZone() const { return kind_ == AtomsZone; }
  bool isSelfHostingZone() const { return kind_ == SelfHostingZone; }
  bool isSystemZone() const { return kind_ == SystemZone; }

  static shadow::Zone* from(JS::Zone* zone) {
    return reinterpret_cast<shadow::Zone*>(zone);
  }
};

}  // namespace shadow

}  // namespace JS

#endif  // js_shadow_Zone_h
