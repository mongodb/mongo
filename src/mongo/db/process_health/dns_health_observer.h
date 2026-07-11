// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_observer_base.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

namespace mongo {
namespace process_health {
class DnsHealthObserver final : public HealthObserverBase {
public:
    DnsHealthObserver(ServiceContext* svcCtx)
        : HealthObserverBase(svcCtx), _random(PseudoRandom(SecureRandom().nextInt64())) {};

protected:
    FaultFacetType getType() const override {
        return FaultFacetType::kDns;
    }

    Milliseconds healthCheckJitter() const override {
        return Milliseconds(5);
    }

    Milliseconds getObserverTimeout() const override {
        return Milliseconds(Seconds(10));
    }

    bool isConfigured() const override {
        return true;
    }

    Future<HealthCheckStatus> periodicCheckImpl(
        PeriodicHealthCheckContext&& periodicCheckContext) override;

private:
    mutable PseudoRandom _random;
};
}  // namespace process_health
}  // namespace mongo
