// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_observer_base.h"
#include "mongo/db/service_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

namespace mongo {
namespace process_health {
class TestHealthObserver : public HealthObserverBase {
public:
    TestHealthObserver(ServiceContext* svcCtx) : HealthObserverBase(svcCtx) {};

protected:
    FaultFacetType getType() const override {
        return FaultFacetType::kTestObserver;
    }

    Milliseconds healthCheckJitter() const override {
        return Milliseconds(0);
    }

    Milliseconds getObserverTimeout() const override {
        return Seconds(30);
    }

    Future<HealthCheckStatus> periodicCheckImpl(
        PeriodicHealthCheckContext&& periodicCheckContext) override;

    bool isConfigured() const override;
};
}  // namespace process_health
}  // namespace mongo
