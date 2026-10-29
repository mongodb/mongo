// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/util/modules.h"

#include <absl/base/internal/cycleclock.h>

namespace mongo::tracing_profiler::internal {

/**
 * A native system clock that returns current time and frequency.
 */
// TODO(SERVER-90115): Replace internal absl cycleclock access with non-internal and officially
// supported cycleclock api.
// The internal absl cycleclock is used temporarily as a workaround for
// lack of cycleclock api, this should we fixed once the SERVER-90115 is done.
struct SystemCycleClock {
    MONGO_COMPILER_ALWAYS_INLINE int64_t now() {
        return absl::base_internal::CycleClock::Now();
    }

    MONGO_COMPILER_ALWAYS_INLINE int64_t frequency() {
        return absl::base_internal::CycleClock::Frequency();
    }

    static SystemCycleClock& get();

private:
    friend class CycleClockSource;
};

/**
 * A generic abstracted interface of a system clock that can be used for testing.
 * SystemCycleClock doesn't implement this interface as we don't want to introduce any indirection
 * when performing actual benchmarking.
 */
class CycleClockIface {
public:
    virtual int64_t now() = 0;
    virtual double frequency() = 0;
};

}  // namespace mongo::tracing_profiler::internal
