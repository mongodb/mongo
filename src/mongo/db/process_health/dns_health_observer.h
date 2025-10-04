/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
