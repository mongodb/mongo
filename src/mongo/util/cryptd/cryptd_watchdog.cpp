/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/util/cryptd/cryptd_watchdog.h"

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/time_support.h"
#include "mongo/watchdog/watchdog.h"

#include <cstdint>
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

/**
 * Number of times we have to see the generation number not change before we shutdown.
 *
 * In order to get a reasonable accurate timeout, we take the idle watchdog timeout the user set
 * and divide-by kMissedCounts. This means that we can take an extra (1/kMissedCounts * interval)
 * before we realize we should shutdown.
 */
constexpr std::int64_t kMissedCounts = 10;

class IdleWatchdogThread : public WatchdogPeriodicThread {
public:
    IdleWatchdogThread(Milliseconds interval, transport::TransportLayerManager* tlm)
        : WatchdogPeriodicThread(interval / kMissedCounts, "idleWatchdog"),
          _userTimeout(duration_cast<Seconds>(interval)),
          _transportLayerManager(tlm) {}

    /**
     * Signal the idle watchdog a new connection was made.
     */
    void signal() {
        _generation.fetchAndAdd(1);
    }

    /**
     * Return true if we are in shutdown to prevent recursion in shutdown.
     */
    bool inShutdown() const {
        return _inShutdown.load();
    }

private:
    void run(OperationContext* opCtx) final {
        // Should we shutdown?
        // 1. Check that there are no connections
        // 2. Check that there have not been any connections recently
        //    - There could have been transient connections
        // 3. Check that been idle for too long

        if (_transportLayerManager && _transportLayerManager->hasActiveSessions()) {
            return;
        }

        // Check if the generation counter has been bumped
        auto currentGeneration = _generation.load();
        if (_lastSeenGeneration != currentGeneration) {
            _missedCounter = 0;
            _lastSeenGeneration = currentGeneration;
            return;
        }

        ++_missedCounter;

        // If we have seen the generation count bump in N runs, exit
        if (_missedCounter >= kMissedCounts) {
            LOGV2(24237,
                  "Mongocryptd has not received a command in the expected timeout window, exiting",
                  "userTimeout"_attr = _userTimeout);
            _inShutdown.store(true);
            exitCleanly(ExitCode::kill);
        }
    }
    void resetState() final {
        _missedCounter = 0;
        _lastSeenGeneration = _generation.load();
    }

private:
    // A generation number that increases on each new connection.
    AtomicWord<long long> _generation{0};

    // Number of seconds user asked to wait after going idle to wait before shuting down.
    Seconds _userTimeout;

    // Number of intervals we have been idle.
    std::uint32_t _missedCounter{0};

    // The last connection number we have seen.
    std::int64_t _lastSeenGeneration{0};

    transport::TransportLayerManager* _transportLayerManager;

    // A flag used to avoid recursive watchdog shutdown.
    AtomicWord<bool> _inShutdown{false};
};

const auto getIdleWatchdogMonitor =
    ServiceContext::declareDecoration<std::unique_ptr<IdleWatchdogThread>>();

}  // namespace

void startIdleWatchdog(ServiceContext* svcCtx, Seconds timeout) {
    // Only setup the watchdog if the timeout is > 0
    if (timeout == Seconds(0)) {
        return;
    }

    auto watchdog =
        std::make_unique<IdleWatchdogThread>(timeout, svcCtx->getTransportLayerManager());

    watchdog->start();

    getIdleWatchdogMonitor(svcCtx) = std::move(watchdog);
}

void signalIdleWatchdog() {
    auto watchdog = getIdleWatchdogMonitor(getGlobalServiceContext()).get();

    if (watchdog) {
        watchdog->signal();
    }
}

void shutdownIdleWatchdog(ServiceContext* serviceContext) {
    auto watchdog = getIdleWatchdogMonitor(serviceContext).get();

    // Only call watchdog shutdown when not already in a watchdog triggered shutdown.
    if (watchdog && !watchdog->inShutdown()) {
        watchdog->shutdown();
    }
}

}  // namespace mongo
