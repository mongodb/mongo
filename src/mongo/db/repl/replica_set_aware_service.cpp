/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        service->onStartup(opCtx);
    });
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
