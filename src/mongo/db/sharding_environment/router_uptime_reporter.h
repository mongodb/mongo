// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <string>

namespace mongo {

/**
 * Utility class, which is used to periodically record in the config server's metadata the router
 * instances, which are connected to the given config server and their uptime.
 *
 * NOTE: Not thread-safe, so it should not be used from more than one thread at a time.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] RouterUptimeReporter {
    RouterUptimeReporter(const RouterUptimeReporter&) = delete;
    RouterUptimeReporter& operator=(const RouterUptimeReporter&) = delete;

public:
    RouterUptimeReporter() = default;
    ~RouterUptimeReporter();

    static RouterUptimeReporter& get(ServiceContext* serviceContext);

    /**
     * Optional call, which would start a thread to periodically invoke reportStatus.
     */
    void startPeriodicThread(ServiceContext* serviceContext);

    /**
     * Signals the uptime reporter thread to stop and joins it. Safe to call multiple times.
     */
    void shutdown();

private:
    // The background uptime reporter thread (if started)
    stdx::thread _thread;
};

}  // namespace mongo
