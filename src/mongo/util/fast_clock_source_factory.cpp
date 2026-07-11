// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/fast_clock_source_factory.h"

#include "mongo/util/background_thread_clock_source.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/system_clock_source.h"

#include <memory>

namespace mongo {

std::unique_ptr<ClockSource> FastClockSourceFactory::create(Milliseconds granularity) {
    // TODO: Create the fastest to read wall clock available on the system.
    // For now, assume there is no built-in fast wall clock so instead
    // create a background-thread-based timer.
    return std::make_unique<BackgroundThreadClockSource>(std::make_unique<SystemClockSource>(),
                                                         granularity);
}

}  // namespace mongo
