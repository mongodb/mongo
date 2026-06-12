/**
 * @file mlib/timer.h
 * @brief Timer types and functions
 * @date 2025-04-18
 *
 * This file contains APIs for creating fixed-deadline timer objects that represent
 * stable expiration points.
 *
 * @copyright Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef MLIB_TIMER_H_INCLUDED
#define MLIB_TIMER_H_INCLUDED

#include <mlib/config.h>
#include <mlib/duration.h>
#include <mlib/time_point.h>

mlib_extern_c_begin();

/**
 * @brief Represents an expiry timer. The timer stores some point-in-time
 * after which it is considered to have "expired."
 */
typedef struct mlib_timer {
   /**
    * @brief The point-in-time at which the timer will be considered expired.
    *
    * This field can be updated or modified to change the expiration time of
    * the timer.
    */
   mlib_time_point expires_at;
} mlib_timer;

/**
 * @brief Create a deadline timer that expires at the given point-in-time
 *
 * @param t The point-in-time at which the returned timer should be expired
 * @return mlib_timer
 */
static inline mlib_timer
mlib_expires_at(const mlib_time_point t) mlib_noexcept
{
   mlib_timer ret;
   ret.expires_at = t;
   return ret;
}

/**
 * @brief Create a deadline timer that expires after the given duration has
 * elapsed from the point-in-time at which this function is called
 */
static inline mlib_timer
mlib_expires_after(const mlib_duration dur) mlib_noexcept
{
   const mlib_time_point later = mlib_time_add(mlib_now(), dur);
   return mlib_expires_at(later);
}

#define mlib_expires_after(...) mlib_expires_after(mlib_duration(__VA_ARGS__))

/**
 * @brief Obtain a timer that will "never" expire
 *
 * In actuality, the timer expires at a time so far in the future that no computer
 * program could ever hope to continue running to that point, and by the time
 * that point is reached it will be some other civilization's problem.
 */
static inline mlib_timer
mlib_expires_never(void) mlib_noexcept
{
   mlib_timer t;
   t.expires_at.time_since_monotonic_start = mlib_duration_max();
   return t;
}

/**
 * @brief Between two timers, return the timer that will expire the soonest
 */
static inline mlib_timer
mlib_soonest_timer(mlib_timer l, mlib_timer r) mlib_noexcept
{
   l.expires_at = mlib_earliest(l.expires_at, r.expires_at);
   return l;
}

/**
 * @brief Obtain the duration of time that is remaining until the given timer
 * expires. If the timer has expired, the returned duration will be zero (never
 * negative)
 */
static inline mlib_duration
mlib_timer_remaining(const mlib_timer timer) mlib_noexcept
{
   // The duration until the expiry time of the timer
   const mlib_duration remain = mlib_time_difference(timer.expires_at, mlib_now());
   if (mlib_duration_cmp(remain, <, mlib_duration())) {
      // No time remaining. Return a zero duration (not a negative duration)
      return mlib_duration();
   }
   return remain;
}

/**
 * @brief Test for timer expiration
 *
 * @param timer The timer to be tested
 * @param once (Optional) A pointer to an optional once-flag that will be set
 * to `true` (see below)
 *
 * The function behaves as follows:
 *
 * - If `once` is a null pointer, then returns a boolean indicating whether the
 *   timer has expired.
 * - Otherwise, if `*once` is `true`:
 *   - If `timer` has expired, returns `true`
 *   - Otherwise, `*once` is set to `true` and returns `false`
 *
 * The intent of the `once` flag is to support loops that check for expiry,
 * where at least one iteration of the loop *must* be attempted, even if the
 * timer has expired. For example:
 *
 * ```
 *      void do_thing() {
 *          bool once = false;
 *          while (!mlib_timer_is_expired(timer, &once)) {
 *              try_thing(timer);
 *          }
 *      }
 * ```
 *
 * In the above, `try_thing` will be called *at least once*, even if the timer
 * is already expired.
 */
static inline bool
mlib_timer_is_expired(const mlib_timer timer, bool *once) mlib_noexcept
{
   // Is the timer already expired?
   const bool no_time_remaining = mlib_time_cmp(timer.expires_at, <=, mlib_now());
   if (!once) {
      // Just return `true` if there is zero time remaining
      return no_time_remaining;
   } else {
      // Tweak behavior based on the `*once` value
      if (!*once) {
         // This is the first time we have been called with the given once-flag
         *once = true;
         // Don't count an expiration, even if we have zero time left, because
         // the caller wants to try some operation at least once
         return false;
      }
      return no_time_remaining;
   }
}

mlib_extern_c_end();

#define mlib_timer_is_expired(...) MLIB_ARGC_PICK(_mlibTimerIsExpired, __VA_ARGS__)
#define _mlibTimerIsExpired_argc_1(Timer) mlib_timer_is_expired((Timer), NULL)
#define _mlibTimerIsExpired_argc_2(Timer, OncePtr) mlib_timer_is_expired((Timer), (OncePtr))

#endif // MLIB_TIMER_H_INCLUDED
