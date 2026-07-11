// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/tracing_profiler/internal/cycleclock.h"
namespace mongo::tracing_profiler::internal {

SystemCycleClock& SystemCycleClock::get() {
    static SystemCycleClock instance;
    return instance;
}

}  // namespace mongo::tracing_profiler::internal
