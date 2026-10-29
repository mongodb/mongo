// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/timer.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"

#include <cstdint>

namespace mongo {

namespace {

const int64_t kMicrosPerSecond = 1000 * 1000;

}  // unnamed namespace

Timer::Timer() : Timer(globalSystemTickSource()) {}

Timer::Timer(TickSource* tickSource)
    : _tickSource(tickSource),
      _microsPerCount(static_cast<double>(kMicrosPerSecond) / _tickSource->getTicksPerSecond()) {
    reset();
}

long long Timer::_now() const {
    return _tickSource->getTicks();
}

}  // namespace mongo
