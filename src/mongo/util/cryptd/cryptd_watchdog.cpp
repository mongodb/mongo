// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/util/cryptd/cryptd_watchdog.h"

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
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
    Atomic<long long> _generation{0};

    // Number of seconds user asked to wait after going idle to wait before shuting down.
    Seconds _userTimeout;

    // Number of intervals we have been idle.
    std::uint32_t _missedCounter{0};

    // The last connection number we have seen.
    std::int64_t _lastSeenGeneration{0};

    transport::TransportLayerManager* _transportLayerManager;

    // A flag used to avoid recursive watchdog shutdown.
    Atomic<bool> _inShutdown{false};
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
