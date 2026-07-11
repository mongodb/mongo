// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>

namespace mongo {

class ClockSource;

// Factory that creates the fastest to read wall clock available on the system.
class FastClockSourceFactory {
public:
    /**
     * Creates the fastest to read wall clock available on the system.
     * However, on systems with no built-in fast wall clock,
     * creates a background-thread-based clock implementation.
     */
    static std::unique_ptr<ClockSource> create(Milliseconds granularity);
};

}  // namespace mongo
