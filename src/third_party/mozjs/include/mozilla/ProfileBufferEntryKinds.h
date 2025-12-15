/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferEntryKinds_h
#define ProfileBufferEntryKinds_h

#include "mozilla/BaseProfilerUtils.h"

#include <cstdint>

namespace mozilla {

// This is equal to sizeof(double), which is the largest non-char variant in
// |u|.
static constexpr size_t ProfileBufferEntryNumChars = 8;

// NOTE!  If you add entries, you need to verify if they need to be added to the
// switch statement in DuplicateLastSample!
// This will evaluate the MACRO with (KIND, TYPE, SIZE)
#define FOR_EACH_PROFILE_BUFFER_ENTRY_KIND(MACRO)                 \
  MACRO(CategoryPair, int, sizeof(int))                           \
  MACRO(CollectionStart, double, sizeof(double))                  \
  MACRO(CollectionEnd, double, sizeof(double))                    \
  MACRO(Label, const char*, sizeof(const char*))                  \
  MACRO(FrameFlags, uint64_t, sizeof(uint64_t))                   \
  MACRO(DynamicStringFragment, char*, ProfileBufferEntryNumChars) \
  MACRO(JitReturnAddr, void*, sizeof(void*))                      \
  MACRO(InnerWindowID, uint64_t, sizeof(uint64_t))                \
  MACRO(LineNumber, int, sizeof(int))                             \
  MACRO(ColumnNumber, int, sizeof(int))                           \
  MACRO(NativeLeafAddr, void*, sizeof(void*))                     \
  MACRO(Pause, double, sizeof(double))                            \
  MACRO(Resume, double, sizeof(double))                           \
  MACRO(PauseSampling, double, sizeof(double))                    \
  MACRO(ResumeSampling, double, sizeof(double))                   \
  MACRO(Responsiveness, double, sizeof(double))                   \
  MACRO(ThreadId, ::mozilla::baseprofiler::BaseProfilerThreadId,  \
        sizeof(::mozilla::baseprofiler::BaseProfilerThreadId))    \
  MACRO(Time, double, sizeof(double))                             \
  MACRO(TimeBeforeCompactStack, double, sizeof(double))           \
  MACRO(TimeBeforeSameSample, double, sizeof(double))             \
  MACRO(CounterId, void*, sizeof(void*))                          \
  MACRO(Number, uint64_t, sizeof(uint64_t))                       \
  MACRO(Count, int64_t, sizeof(int64_t))                          \
  MACRO(ProfilerOverheadTime, double, sizeof(double))             \
  MACRO(ProfilerOverheadDuration, double, sizeof(double))

// The `Kind` is a single byte identifying the type of data that is actually
// stored in a `ProfileBufferEntry`, as per the list in
// `FOR_EACH_PROFILE_BUFFER_ENTRY_KIND`.
//
// This byte is also used to identify entries in ProfileChunkedBuffer blocks,
// for both "legacy" entries that do contain a `ProfileBufferEntry`, and for
// new types of entries that may carry more data of different types.
// TODO: Eventually each type of "legacy" entry should be replaced with newer,
// more efficient kinds of entries (e.g., stack frames could be stored in one
// bigger entry, instead of multiple `ProfileBufferEntry`s); then we could
// discard `ProfileBufferEntry` and move this enum to a more appropriate spot.
enum class ProfileBufferEntryKind : uint8_t {
  INVALID = 0,
#define KIND(KIND, TYPE, SIZE) KIND,
  FOR_EACH_PROFILE_BUFFER_ENTRY_KIND(KIND)
#undef KIND

  // Any value under `LEGACY_LIMIT` represents a `ProfileBufferEntry`.
  LEGACY_LIMIT,

  // Any value starting here does *not* represent a `ProfileBufferEntry` and
  // requires separate decoding and handling.

  // Markers and their data.
  Marker = LEGACY_LIMIT,

  // Entry with "running times", such as CPU usage measurements.
  // Optional between TimeBeforeX and X.
  RunningTimes,

  // Optional between TimeBeforeX and X.
  UnresponsiveDurationMs,

  // Collection of legacy stack entries, must follow a ThreadId and
  // TimeBeforeCompactStack (which are not included in the CompactStack;
  // TimeBeforeCompactStack is equivalent to Time, but indicates that a
  // CompactStack follows shortly afterwards).
  CompactStack,

  // Indicates that this sample is identical to the previous one, must follow a
  // ThreadId and TimeBeforeSameSample.
  SameSample,

  MODERN_LIMIT
};

enum class MarkerPayloadType : uint8_t {
  Cpp,
  Rust,
};

}  // namespace mozilla

#endif  // ProfileBufferEntryKinds_h
