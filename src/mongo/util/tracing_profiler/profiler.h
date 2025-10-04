/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
