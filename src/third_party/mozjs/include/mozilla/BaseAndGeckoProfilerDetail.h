/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Internal Base and Gecko Profiler utilities.
// It should declare or define things that are used in both profilers, but not
// needed outside of the profilers.
// In particular, it is *not* included in popular headers like BaseProfiler.h
// and GeckoProfiler.h, to avoid rebuilding the world when this is modified.

#ifndef BaseAndGeckoProfilerDetail_h
#define BaseAndGeckoProfilerDetail_h

#include "mozilla/BaseProfilerUtils.h"
#include "mozilla/Span.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Types.h"
#include "mozilla/UniquePtr.h"

namespace mozilla {

class ProfileBufferChunkManagerWithLocalLimit;

// Centrally defines the version of the gecko profiler JSON format.
const int GECKO_PROFILER_FORMAT_VERSION = 31;

namespace baseprofiler::detail {

[[nodiscard]] MFBT_API TimeStamp GetProfilingStartTime();

[[nodiscard]] MFBT_API UniquePtr<ProfileBufferChunkManagerWithLocalLimit>
ExtractBaseProfilerChunkManager();

// If the current thread is registered, returns its registration time, otherwise
// a null timestamp.
[[nodiscard]] MFBT_API TimeStamp GetThreadRegistrationTime();

}  // namespace baseprofiler::detail

namespace profiler::detail {

// True if the filter is exactly "pid:<aPid>".
[[nodiscard]] MFBT_API bool FilterHasPid(
    const char* aFilter, baseprofiler::BaseProfilerProcessId aPid =
                             baseprofiler::profiler_current_process_id());

// Only true if the filters only contain "pid:..." strings, and *none* of them
// is exactly "pid:<aPid>". E.g.:
// - [], 123                     -> false (no pids)
// - ["main"], 123               -> false (not all pids)
// - ["main", "pid:123"], 123    -> false (not all pids)
// - ["pid:123"], 123            -> false (all pids, including "pid:123")
// - ["pid:123", "pid:456"], 123 -> false (all pids, including "pid:123")
// - ["pid:456"], 123            -> true (all pids, but no "pid:123")
// - ["pid:456", "pid:789"], 123 -> true (all pids, but no "pid:123")
[[nodiscard]] MFBT_API bool FiltersExcludePid(
    Span<const char* const> aFilters,
    baseprofiler::BaseProfilerProcessId aPid =
        baseprofiler::profiler_current_process_id());

}  // namespace profiler::detail

}  // namespace mozilla

#endif  // BaseAndGeckoProfilerDetail_h
