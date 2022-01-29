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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <functional>

#include "mongo/db/process_health/fault_facet.h"

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/timer.h"

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

    ~FaultFacetMock() = default;

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
