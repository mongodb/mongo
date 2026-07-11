// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/util/aligned.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/tracing_profiler/internal/profiler_internal.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include <absl/container/flat_hash_map.h>
#include <boost/align/aligned_alloc.hpp>
#include <boost/align/aligned_allocator.hpp>
#include <boost/align/aligned_delete.hpp>

#pragma once

namespace mongo::tracing_profiler {

#if MONGO_CONFIG_USE_TRACING_PROFILER

typedef internal::ProfilerSpan ProfilerSpan;

#define MONGO_PROFILER_SPAN_ENTER(spanName) \
    (::mongo::tracing_profiler::internal::GlobalProfilerService::enterSpan<spanName>())

#define MONGO_PROFILER_SPAN_LEAVE(span) span.release()

#else
struct [[maybe_unused]] ProfilerSpan {};

#define MONGO_PROFILER_SPAN_ENTER(spanName) (::mongo::tracing_profiler::ProfilerSpan())

#define MONGO_PROFILER_SPAN_LEAVE(spanGuard)

#endif

}  // namespace mongo::tracing_profiler
