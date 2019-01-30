/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "mozilla/PlatformConditionVariable.h"
#include "mozilla/PlatformMutex.h"
#include "MutexPlatformData_posix.h"

using mozilla::CheckedInt;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

static const long NanoSecPerSec = 1000000000;

// Android 32-bit & macOS 10.12 has the clock functions, but not pthread_condattr_setclock.
#if defined(HAVE_CLOCK_MONOTONIC) && \
    !(defined(__ANDROID__) && !defined(__LP64__)) && !defined(__APPLE__)
# define CV_USE_CLOCK_API
#endif

#ifdef CV_USE_CLOCK_API
// The C++ specification defines std::condition_variable::wait_for in terms of
// std::chrono::steady_clock, which is closest to CLOCK_MONOTONIC.
static const clockid_t WhichClock = CLOCK_MONOTONIC;

// While timevaladd is widely available to work with timevals, the newer
// timespec structure is largely lacking such conveniences. Thankfully, the
// utilities available in MFBT make implementing our own quite easy.
static void
moz_timespecadd(struct timespec* lhs, struct timespec* rhs, struct timespec* result)
{
  // Add nanoseconds. This may wrap, but not above 2 billion.
  MOZ_RELEASE_ASSERT(lhs->tv_nsec < NanoSecPerSec);
  MOZ_RELEASE_ASSERT(rhs->tv_nsec < NanoSecPerSec);
  result->tv_nsec = lhs->tv_nsec + rhs->tv_nsec;

  // Add seconds, checking for overflow in the platform specific time_t type.
  CheckedInt<time_t> sec = CheckedInt<time_t>(lhs->tv_sec) + rhs->tv_sec;

  // If nanoseconds overflowed, carry the result over into seconds.
  if (result->tv_nsec >= NanoSecPerSec) {
    MOZ_RELEASE_ASSERT(result->tv_nsec < 2 * NanoSecPerSec);
    result->tv_nsec -= NanoSecPerSec;
    sec += 1;
  }

  // Extracting the value asserts that there was no overflow.
  MOZ_RELEASE_ASSERT(sec.isValid());
  result->tv_sec = sec.value();
}
#endif

struct mozilla::detail::ConditionVariableImpl::PlatformData
{
  pthread_cond_t ptCond;
};

mozilla::detail::ConditionVariableImpl::ConditionVariableImpl()
{
  pthread_cond_t* ptCond = &platformData()->ptCond;

#ifdef CV_USE_CLOCK_API
  pthread_condattr_t attr;
  int r0 = pthread_condattr_init(&attr);
  MOZ_RELEASE_ASSERT(!r0);

  int r1 = pthread_condattr_setclock(&attr, WhichClock);
  MOZ_RELEASE_ASSERT(!r1);

  int r2 = pthread_cond_init(ptCond, &attr);
  MOZ_RELEASE_ASSERT(!r2);

  int r3 = pthread_condattr_destroy(&attr);
  MOZ_RELEASE_ASSERT(!r3);
#else
  int r = pthread_cond_init(ptCond, NULL);
  MOZ_RELEASE_ASSERT(!r);
#endif
}

mozilla::detail::ConditionVariableImpl::~ConditionVariableImpl()
{
  int r = pthread_cond_destroy(&platformData()->ptCond);
  MOZ_RELEASE_ASSERT(r == 0);
}

void
mozilla::detail::ConditionVariableImpl::notify_one()
{
  int r = pthread_cond_signal(&platformData()->ptCond);
  MOZ_RELEASE_ASSERT(r == 0);
}

void
mozilla::detail::ConditionVariableImpl::notify_all()
{
  int r = pthread_cond_broadcast(&platformData()->ptCond);
  MOZ_RELEASE_ASSERT(r == 0);
}

void
mozilla::detail::ConditionVariableImpl::wait(MutexImpl& lock)
{
  pthread_cond_t* ptCond = &platformData()->ptCond;
  pthread_mutex_t* ptMutex = &lock.platformData()->ptMutex;

  int r = pthread_cond_wait(ptCond, ptMutex);
  MOZ_RELEASE_ASSERT(r == 0);
}

mozilla::detail::CVStatus
mozilla::detail::ConditionVariableImpl::wait_for(MutexImpl& lock,
						 const TimeDuration& a_rel_time)
{
  if (a_rel_time == TimeDuration::Forever()) {
    wait(lock);
    return CVStatus::NoTimeout;
  }

  pthread_cond_t* ptCond = &platformData()->ptCond;
  pthread_mutex_t* ptMutex = &lock.platformData()->ptMutex;
  int r;

  // Clamp to 0, as time_t is unsigned.
  TimeDuration rel_time = a_rel_time < TimeDuration::FromSeconds(0)
                          ? TimeDuration::FromSeconds(0)
                          : a_rel_time;

  // Convert the duration to a timespec.
  struct timespec rel_ts;
  rel_ts.tv_sec = static_cast<time_t>(rel_time.ToSeconds());
  rel_ts.tv_nsec = static_cast<uint64_t>(rel_time.ToMicroseconds() * 1000.0) % NanoSecPerSec;

#ifdef CV_USE_CLOCK_API
  struct timespec now_ts;
  r = clock_gettime(WhichClock, &now_ts);
  MOZ_RELEASE_ASSERT(!r);

  struct timespec abs_ts;
  moz_timespecadd(&now_ts, &rel_ts, &abs_ts);

  r = pthread_cond_timedwait(ptCond, ptMutex, &abs_ts);
#else
  // Our non-clock-supporting platforms, OS X and Android, do support waiting
  // on a condition variable with a relative timeout.
  r = pthread_cond_timedwait_relative_np(ptCond, ptMutex, &rel_ts);
#endif

  if (r == 0) {
    return CVStatus::NoTimeout;
  }
  MOZ_RELEASE_ASSERT(r == ETIMEDOUT);
  return CVStatus::Timeout;
}

mozilla::detail::ConditionVariableImpl::PlatformData*
mozilla::detail::ConditionVariableImpl::platformData()
{
  static_assert(sizeof platformData_ >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<PlatformData*>(platformData_);
}
