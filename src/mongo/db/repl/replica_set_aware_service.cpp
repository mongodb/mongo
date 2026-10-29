// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/replica_set_aware_service.h"

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

auto registryDecoration = ServiceContext::declareDecoration<ReplicaSetAwareServiceRegistry>();

}  // namespace

ReplicaSetAwareServiceRegistry::~ReplicaSetAwareServiceRegistry() {
    invariant(_services.empty());
}

ReplicaSetAwareServiceRegistry& ReplicaSetAwareServiceRegistry::get(
    ServiceContext* serviceContext) {
    return registryDecoration(serviceContext);
}

void ReplicaSetAwareServiceRegistry::onStartup(OperationContext* opCtx) {
    // Only start up services if we are in uninitialized state: i.e., shutdown has not been called.
    int expected = kUninitialized;
    if (!_state.compareAndSwap(&expected, kStartup)) {
        LOGV2(12968800,
              "Skipping ReplicaSetAwareServiceRegistry startup because it has already run or "
              "shutdown has already been called",
              "state"_attr = expected);
        return;
    }

    // Publish the terminal startup state on exit so that a concurrent onShutdown waiting for
    // startup to finish can make progress.
    State exitState = kStarted;
    ON_BLOCK_EXIT([&] {
        invariant(_state.swap(exitState) == kStartup);
        _state.notifyAll();
    });

    for (auto* service : _services) {
        // Stop starting services as soon as a shutdown has been requested so shutdown does not have
        // to wait for the rest of the services to start.
        if (MONGO_unlikely(_inShutdown.load())) {
            LOGV2(12968801,
                  "Interrupting ReplicaSetAwareServiceRegistry startup because a shutdown was "
                  "requested");
            exitState = kStartupAborted;
            return;
        }
        service->onStartup(opCtx);
    }
}

void ReplicaSetAwareServiceRegistry::onSetCurrentConfig(OperationContext* opCtx) {
    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        service->onSetCurrentConfig(opCtx);
    });
}

void ReplicaSetAwareServiceRegistry::onConsistentDataAvailable(OperationContext* opCtx,
                                                               bool isMajority,
                                                               bool isRollback) {
    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        service->onConsistentDataAvailable(opCtx, isMajority, isRollback);
    });
}

void ReplicaSetAwareServiceRegistry::onShutdown() {
    // Record that a shutdown has been requested so that, if services are still starting up, we can
    // stop the startup process and proceed to shutting down.
    if (_inShutdown.swap(true)) {
        return;
    }

    // Claim the shutdown by moving to kShutdown, but only if we are not currently starting up.
    // If we observe kStartup, wait for that startup to complete (transition to kStarted or
    // kStartupAborted) and retry the exchange. This guarantees onShutdown never runs a service's
    // shutdown hook before or concurrently with its onStartup.
    while (true) {
        auto current = _state.load();
        if (current == kStartup) {
            _state.wait(kStartup);
            continue;
        }

        // We have not begun to startup, we have completed startup, or we have aborted the startup
        // process.
        invariant(current == kUninitialized || current == kStarted || current == kStartupAborted);

        // Protect from race with uninitialized -> startup transition in onStartup.
        if (_state.compareAndSwap(&current, kShutdown)) {
            break;
        }
    }

    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        service->onShutdown();
    });
}

void ReplicaSetAwareServiceRegistry::onStepUpBegin(OperationContext* opCtx, long long term) {
    // Since this method is run during drain mode and can block state transition, generate a warning
    // if we are spending too long here.
    Timer totalTime{};
    ON_BLOCK_EXIT([&] {
        auto timeSpent = totalTime.millis();
        auto threshold = repl::slowTotalOnStepUpBeginThresholdMS.load();
        if (timeSpent > threshold) {
            LOGV2(
                6699600,
                "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpBegin for all services "
                "exceeded slowTotalOnStepUpBeginThresholdMS",
                "thresholdMillis"_attr = threshold,
                "durationMillis"_attr = timeSpent);
        }
    });

    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        // Additionally, generate a warning if any individual service is taking too long.
        Timer t{};
        ON_BLOCK_EXIT([&] {
            auto timeSpent = t.millis();
            auto threshold = repl::slowServiceOnStepUpBeginThresholdMS.load();
            if (timeSpent > threshold) {
                LOGV2(6699601,
                      "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpBegin "
                      "for service exceeded slowServiceOnStepUpBeginThresholdMS",
                      "thresholdMillis"_attr = threshold,
                      "durationMillis"_attr = timeSpent,
                      "serviceName"_attr = service->getServiceName());
            }
        });
        service->onStepUpBegin(opCtx, term);
    });
}

void ReplicaSetAwareServiceRegistry::onStepUpComplete(OperationContext* opCtx, long long term) {
    // Since this method is called before we mark the node writable in
    // ReplicationCoordinatorImpl::signalApplierDrainComplete and therefore can block the new
    // primary from starting to receive writes, generate a warning if we are spending too long here.
    Timer totalTime{};
    ON_BLOCK_EXIT([&] {
        auto timeSpent = totalTime.millis();
        auto threshold = repl::slowTotalOnStepUpCompleteThresholdMS.load();
        if (timeSpent > threshold) {
            LOGV2(6699602,
                  "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpComplete "
                  "for all services exceeded slowTotalOnStepUpCompleteThresholdMS",
                  "thresholdMillis"_attr = threshold,
                  "durationMillis"_attr = timeSpent);
        }
    });

    LOGV2(8025900, "ReplicaSetAwareServiceRegistry::onStepUpComplete stepping up all services");

    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        // Additionally, generate a warning if any individual service is taking too long.
        Timer t{};
        ON_BLOCK_EXIT([&] {
            auto timeSpent = t.millis();
            auto threshold = repl::slowServiceOnStepUpCompleteThresholdMS.load();
            if (timeSpent > threshold) {
                LOGV2(6699603,
                      "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpComplete "
                      "for service exceeded slowServiceOnStepUpCompleteThresholdMS",
                      "thresholdMillis"_attr = threshold,
                      "durationMillis"_attr = timeSpent,
                      "serviceName"_attr = service->getServiceName());
            }
        });
        LOGV2_DEBUG(
            8025901, 1, "Stepping up service", "serviceName"_attr = service->getServiceName());
        service->onStepUpComplete(opCtx, term);
    });
}

void ReplicaSetAwareServiceRegistry::onStepDown() {
    std::for_each(_services.begin(), _services.end(), [](ReplicaSetAwareInterface* service) {
        Timer t{};
        service->onStepDown();

        auto timeSpent = t.millis();
        auto threshold = repl::slowServiceOnStepDownThresholdMS.load();
        if (timeSpent > threshold) {
            LOGV2(10594201,
                  "Duration spent in ReplicaSetAwareServiceRegistry::onStepDown "
                  "for service exceeded slowServiceOnStepDownThresholdMS",
                  "thresholdMillis"_attr = threshold,
                  "durationMillis"_attr = timeSpent,
                  "serviceName"_attr = service->getServiceName());
        }
    });
}

void ReplicaSetAwareServiceRegistry::onRollbackBegin() {
    std::for_each(_services.begin(), _services.end(), [](ReplicaSetAwareInterface* service) {
        service->onRollbackBegin();
    });
}

void ReplicaSetAwareServiceRegistry::onBecomeArbiter() {
    std::for_each(_services.begin(), _services.end(), [](ReplicaSetAwareInterface* service) {
        service->onBecomeArbiter();
    });
}

void ReplicaSetAwareServiceRegistry::_registerService(ReplicaSetAwareInterface* service) {
    _services.push_back(service);
}

void ReplicaSetAwareServiceRegistry::_unregisterService(ReplicaSetAwareInterface* service) {
    auto it = std::find(_services.begin(), _services.end(), service);
    invariant(it != _services.end());
    _services.erase(it);
}

}  // namespace mongo
