/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Time_h
#define vm_Time_h

#include "mozilla/TimeStamp.h"

#include <stddef.h>
#include <stdint.h>

#if !JS_HAS_INTL_API
/*
 * Broken down form of 64 bit time value.
 */
struct PRMJTime {
  int32_t tm_usec; /* microseconds of second (0-999999) */
  int8_t tm_sec;   /* seconds of minute (0-59) */
  int8_t tm_min;   /* minutes of hour (0-59) */
  int8_t tm_hour;  /* hour of day (0-23) */
  int8_t tm_mday;  /* day of month (1-31) */
  int8_t tm_mon;   /* month of year (0-11) */
  int8_t tm_wday;  /* 0=sunday, 1=monday, ... */
  int32_t tm_year; /* absolute year, AD */
  int16_t tm_yday; /* day of year (0 to 365) */
  int8_t tm_isdst; /* non-zero if DST in effect */
};
#endif

/* Some handy constants */
#define PRMJ_USEC_PER_SEC 1000000L
#define PRMJ_USEC_PER_MSEC 1000L

/* Return the current local time in micro-seconds */
extern int64_t PRMJ_Now();

#if !JS_HAS_INTL_API
/* Format a time value into a buffer. Same semantics as strftime() */
extern size_t PRMJ_FormatTime(char* buf, size_t buflen, const char* fmt,
                              const PRMJTime* tm, int timeZoneYear,
                              int offsetInSeconds);
#endif

/**
 * Requesting the number of cycles from the CPU.
 *
 * `rdtsc`, or Read TimeStamp Cycle, is an instruction provided by
 * x86-compatible CPUs that lets processes request the number of
 * cycles spent by the CPU executing instructions since the CPU was
 * started. It may be used for performance monitoring, but you should
 * be aware of the following limitations.
 *
 *
 * 1. The value is *not* monotonic.
 *
 * The value is reset to 0 whenever a CPU is turned off (e.g. computer
 * in full hibernation, single CPU going turned off). Moreover, on
 * multi-core/multi-CPU architectures, the cycles of each core/CPU are
 * generally not synchronized.  Therefore, is a process or thread is
 * rescheduled to another core/CPU, the result of `rdtsc` may decrease
 * arbitrarily.
 *
 * The only way to prevent this is to pin your thread to a particular
 * CPU, which is generally not a good idea.
 *
 *
 *
 * 2. The value increases independently.
 *
 * The value may increase whenever the CPU executes an instruction,
 * regardless of the process that has issued this
 * instruction. Moreover, if a process or thread is rescheduled to
 * another core/CPU, the result of `rdtsc` may increase arbitrarily.
 *
 * The only way to prevent this is to ensure that your thread is the
 * sole owner of the CPU. See [1] for an example. This is also
 * generally not a good idea.
 *
 *
 *
 * 3. The value does not measure time.
 *
 * On older architectures (pre-Pentium 4), there was no constant mapping
 * between rdtsc and CPU time.
 *
 *
 * 4. Instructions may be reordered.
 *
 * The CPU can reorder instructions. Also, rdtsc does not necessarily
 * wait until all previous instructions have finished executing before
 * reading the counter. Similarly, subsequent instructions may begin
 * execution before the read operation is performed. If you use rdtsc
 * for micro-benchmarking, you may end up measuring something else
 * than what you expect. See [1] for a study of countermeasures.
 *
 *
 * ** Performance
 *
 * According to unchecked sources on the web, the overhead of rdtsc is
 * expected to be 150-200 cycles on old architectures, 6-50 on newer
 * architectures. Agner's instruction tables [2] seem to confirm the latter
 * results.
 *
 *
 * [1]
 * http://www.intel.com/content/dam/www/public/us/en/documents/white-papers/ia-32-ia-64-benchmark-code-execution-paper.pdf
 * [2] http://www.agner.org/optimize/instruction_tables.pdf
 */

#define MOZ_HAVE_RDTSC 1

#if defined(_WIN32) && (defined(_M_IX86) || defined(_M_AMD64))

#  include <intrin.h>
static __inline uint64_t ReadTimestampCounter(void) { return __rdtsc(); }

#elif defined(__i386__)

static __inline__ uint64_t ReadTimestampCounter(void) {
  uint64_t x;
  __asm__ volatile(".byte 0x0f, 0x31" : "=A"(x));
  return x;
}

#elif defined(__x86_64__)

static __inline__ uint64_t ReadTimestampCounter(void) {
  unsigned hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

#else

#  undef MOZ_HAVE_RDTSC

#endif

namespace js {

class MOZ_RAII AutoIncrementalTimer {
  mozilla::TimeStamp startTime;
  mozilla::TimeDuration& output;

 public:
  AutoIncrementalTimer(const AutoIncrementalTimer&) = delete;
  AutoIncrementalTimer& operator=(const AutoIncrementalTimer&) = delete;

  explicit AutoIncrementalTimer(mozilla::TimeDuration& output_)
      : output(output_) {
    startTime = mozilla::TimeStamp::Now();
  }

  ~AutoIncrementalTimer() { output += mozilla::TimeStamp::Now() - startTime; }
};

}  // namespace js

#endif /* vm_Time_h */
