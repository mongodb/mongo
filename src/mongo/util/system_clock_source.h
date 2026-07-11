// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Clock source based on the system clock.
 */
class SystemClockSource final : public ClockSource {
public:
    Date_t now() final;

    /**
     * Returns the singleton instance of SystemClockSource.
     */
    static SystemClockSource* get();
    Milliseconds getPrecision() override;
};

}  // namespace mongo
