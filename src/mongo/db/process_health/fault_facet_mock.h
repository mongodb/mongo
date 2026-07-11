// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/timer.h"

#include <functional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace process_health {

class FaultFacetMock : public FaultFacet {
public:
    // Testing callback to fill up mocked values.
    using MockCallback = std::function<Severity()>;

    FaultFacetMock(FaultFacetType mockType, ClockSource* clockSource, MockCallback callback)
        : _mockType(mockType), _clockSource(clockSource), _callback(callback) {
        invariant(mockType == FaultFacetType::kMock1 || mockType == FaultFacetType::kMock2);
    }

    ~FaultFacetMock() override = default;

    FaultFacetType getType() const override {
        return _mockType;
    }

    HealthCheckStatus getStatus() const override {
        const Severity severity = _callback();

        auto healthCheckStatus = HealthCheckStatus(_mockType, severity, "Mock facet");
        LOGV2(5956702, "Mock fault facet status", "status"_attr = healthCheckStatus);

        return healthCheckStatus;
    }

    Milliseconds getDuration() const override {
        return std::max(Milliseconds(0), Milliseconds(_clockSource->now() - _startTime));
    }

    void appendDescription(BSONObjBuilder* builder) const override {
        builder->append("type", FaultFacetType_serializer(getType()));
        builder->append("duration", getDuration().toBSON());
    };

    void update(HealthCheckStatus status) override {
        MONGO_UNREACHABLE;  // Don't use this in mock.
    }

private:
    const FaultFacetType _mockType;
    ClockSource* const _clockSource;
    const Date_t _startTime = _clockSource->now();
    const MockCallback _callback;
};

}  // namespace process_health
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
