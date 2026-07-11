// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/watchdog/watchdog_mock.h"

namespace mongo {

void WatchdogMonitorMock::pauseChecks() {
    _shouldRunChecks.store(false);
}

void WatchdogMonitorMock::unpauseChecks() {
    _shouldRunChecks.store(true);
}

void WatchdogMonitorMock::start() {}

void WatchdogMonitorMock::setPeriod(Milliseconds duration) {}

void WatchdogMonitorMock::shutdown() {}

std::int64_t WatchdogMonitorMock::getCheckGeneration() {
    return 0;
}

std::int64_t WatchdogMonitorMock::getMonitorGeneration() {
    return 0;
}

bool WatchdogMonitorMock::getShouldRunChecks_forTest() {
    return _shouldRunChecks.load();
}

}  // namespace mongo
