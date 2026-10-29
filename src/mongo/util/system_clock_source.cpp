// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/system_clock_source.h"

#include "mongo/util/time_support.h"

#include <memory>

namespace mongo {

Date_t SystemClockSource::now() {
    return Date_t::now();
}

SystemClockSource* SystemClockSource::get() {
    static const auto globalSystemClockSource = std::make_unique<SystemClockSource>();
    return globalSystemClockSource.get();
}

Milliseconds SystemClockSource::getPrecision() {
    return Milliseconds(1);
}

}  // namespace mongo
