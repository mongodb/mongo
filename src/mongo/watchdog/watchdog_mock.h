// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/watchdog/watchdog.h"

namespace mongo {

/**
 * A mock WatchdogMonitor for use in C++ unit tests.
 *
 */
class [[MONGO_MOD_PUBLIC]] WatchdogMonitorMock final : public WatchdogMonitorInterface {
public:
    WatchdogMonitorMock() = default;
    ~WatchdogMonitorMock() override = default;

    void pauseChecks() override;

    void unpauseChecks() override;

    void start() override;

    void setPeriod(Milliseconds duration) override;

    void shutdown() override;

    std::int64_t getCheckGeneration() override;

    std::int64_t getMonitorGeneration() override;

    bool getShouldRunChecks_forTest() override;

private:
    Atomic<bool> _shouldRunChecks{true};
};

}  // namespace mongo
