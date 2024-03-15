/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Uptime.h"

#ifdef XP_WIN
#  include "mozilla/DynamicallyLinkedFunctionPtr.h"
#endif  // XP_WIN

#include <stdint.h>

#include "mozilla/TimeStamp.h"
#include "mozilla/Maybe.h"
#include "mozilla/Assertions.h"

using namespace mozilla;

namespace {

Maybe<uint64_t> NowIncludingSuspendMs();
Maybe<uint64_t> NowExcludingSuspendMs();
static Maybe<uint64_t> mStartExcludingSuspendMs;
static Maybe<uint64_t> mStartIncludingSuspendMs;

// Apple things
#if defined(__APPLE__) && defined(__MACH__)
#  include <time.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <mach/mach_time.h>

const uint64_t kNSperMS = 1000000;

Maybe<uint64_t> NowExcludingSuspendMs() {
  return Some(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / kNSperMS);
}

Maybe<uint64_t> NowIncludingSuspendMs() {
  return Some(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / kNSperMS);
}

#elif defined(XP_WIN)

// Number of hundreds of nanoseconds in a millisecond
static constexpr uint64_t kHNSperMS = 10000;

Maybe<uint64_t> NowExcludingSuspendMs() {
  ULONGLONG interrupt_time;
  if (!QueryUnbiasedInterruptTime(&interrupt_time)) {
    return Nothing();
  }
  return Some(interrupt_time / kHNSperMS);
}

Maybe<uint64_t> NowIncludingSuspendMs() {
  static const mozilla::StaticDynamicallyLinkedFunctionPtr<void(WINAPI*)(
      PULONGLONG)>
      pQueryInterruptTime(L"KernelBase.dll", "QueryInterruptTime");
  if (!pQueryInterruptTime) {
    // On Windows, this does include the time the computer was suspended so it's
    // an adequate fallback.
    TimeStamp processCreation = TimeStamp::ProcessCreation();
    TimeStamp now = TimeStamp::Now();
    if (!processCreation.IsNull() && !now.IsNull()) {
      return Some(uint64_t((now - processCreation).ToMilliseconds()));
    } else {
      return Nothing();
    }
  }
  ULONGLONG interrupt_time;
  pQueryInterruptTime(&interrupt_time);
  return Some(interrupt_time / kHNSperMS);
}

#elif defined(XP_UNIX)  // including BSDs and Android
#  include <time.h>

// Number of nanoseconds in a millisecond.
static constexpr uint64_t kNSperMS = 1000000;

uint64_t TimespecToMilliseconds(struct timespec aTs) {
  return aTs.tv_sec * 1000 + aTs.tv_nsec / kNSperMS;
}

Maybe<uint64_t> NowExcludingSuspendMs() {
  struct timespec ts = {0};

#  ifdef XP_OPENBSD
  if (clock_gettime(CLOCK_UPTIME, &ts)) {
#  else
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
#  endif
    return Nothing();
  }
  return Some(TimespecToMilliseconds(ts));
}

Maybe<uint64_t> NowIncludingSuspendMs() {
#  ifndef CLOCK_BOOTTIME
  return Nothing();
#  else
  struct timespec ts = {0};

  if (clock_gettime(CLOCK_BOOTTIME, &ts)) {
    return Nothing();
  }
  return Some(TimespecToMilliseconds(ts));
#  endif
}

#else  // catch all

Maybe<uint64_t> NowExcludingSuspendMs() { return Nothing(); }
Maybe<uint64_t> NowIncludingSuspendMs() { return Nothing(); }

#endif

};  // anonymous namespace

namespace mozilla {

void InitializeUptime() {
  MOZ_RELEASE_ASSERT(mStartIncludingSuspendMs.isNothing() &&
                         mStartExcludingSuspendMs.isNothing(),
                     "Must not be called more than once");
  mStartIncludingSuspendMs = NowIncludingSuspendMs();
  mStartExcludingSuspendMs = NowExcludingSuspendMs();
}

Maybe<uint64_t> ProcessUptimeMs() {
  if (!mStartIncludingSuspendMs) {
    return Nothing();
  }
  Maybe<uint64_t> maybeNow = NowIncludingSuspendMs();
  if (!maybeNow) {
    return Nothing();
  }
  return Some(maybeNow.value() - mStartIncludingSuspendMs.value());
}

Maybe<uint64_t> ProcessUptimeExcludingSuspendMs() {
  if (!mStartExcludingSuspendMs) {
    return Nothing();
  }
  Maybe<uint64_t> maybeNow = NowExcludingSuspendMs();
  if (!maybeNow) {
    return Nothing();
  }
  return Some(maybeNow.value() - mStartExcludingSuspendMs.value());
}

};  // namespace mozilla
