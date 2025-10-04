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
