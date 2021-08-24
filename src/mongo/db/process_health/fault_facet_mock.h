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

#include <functional>

#include "mongo/db/process_health/fault_facet.h"

#include "mongo/db/service_context.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace process_health {

class FaultFacetMock : public FaultFacet {
public:
    // Testing callback to fill up mocked values.
    using MockCallback = std::function<void(double* severity)>;

    FaultFacetMock(ServiceContext* svcCtx, MockCallback callback)
        : _svcCtx(svcCtx), _callback(callback) {}

    ~FaultFacetMock() = default;

    HealthCheckStatus getStatus() const override {
        double severity;
        _callback(&severity);

        auto lk = stdx::lock_guard(_mutex);
        if (HealthCheckStatus::isActiveFault(severity)) {
            if (_activeFaultTime == Date_t::max()) {
                _activeFaultTime = _svcCtx->getFastClockSource()->now();
            }
        } else {
            _activeFaultTime = Date_t::max();
        }

        auto now = _svcCtx->getFastClockSource()->now();
        HealthCheckStatus healthCheckStatus(FaultFacetType::kMock,
                                            severity,
                                            "Mock facet",
                                            now - _activeFaultTime,
                                            now - _startTime);


        return healthCheckStatus;
    }

private:
    ServiceContext* const _svcCtx;
    const Date_t _startTime = _svcCtx->getFastClockSource()->now();
    const MockCallback _callback;

    mutable Mutex _mutex;
    // We also update the _activeFaultTime inside the const getStatus().
    mutable Date_t _activeFaultTime = Date_t::max();
};

}  // namespace process_health
}  // namespace mongo
