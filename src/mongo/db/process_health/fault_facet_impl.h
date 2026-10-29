// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace process_health {

// Internal representation of the Facet.
class FaultFacetImpl : public FaultFacet {
public:
    FaultFacetImpl(FaultFacetType type, ClockSource* clockSource, HealthCheckStatus status);

    ~FaultFacetImpl() override = default;

    // Public interface methods.

    FaultFacetType getType() const override;

    HealthCheckStatus getStatus() const override;

    Milliseconds getDuration() const override;

    void update(HealthCheckStatus status) override;

    void appendDescription(BSONObjBuilder* builder) const override;

private:
    const FaultFacetType _type;
    ClockSource* const _clockSource;

    const Date_t _startTime = _clockSource->now();

    mutable std::mutex _mutex;
    Severity _severity = Severity::kOk;
    std::string _description;
};

}  // namespace process_health
}  // namespace mongo
