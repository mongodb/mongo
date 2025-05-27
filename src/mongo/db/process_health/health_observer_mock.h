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

#include "mongo/db/process_health/fault_facet_mock.h"
#include "mongo/db/process_health/health_observer_base.h"
#include "mongo/logv2/log.h"

#include <functional>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace process_health {

/**
 * Mocked health observer has two modes of operation (depending on constructor called):
 *   1.  Passing a callback that runs on an executor and returns a severity
 *   2.  Passing an implementation of periodicCheckImpl
 *
 * See unit test HealthCheckThrowingExceptionMakesFailedStatus for an example of the second mode.
 */
class HealthObserverMock : public HealthObserverBase {
public:
    HealthObserverMock(FaultFacetType mockType,
                       ServiceContext* svcCtx,
                       std::function<Severity()> getSeverityCallback,
                       Milliseconds observerTimeout)
        : HealthObserverBase(svcCtx),
          _mockType(mockType),
          _getSeverityCallback(getSeverityCallback),
          _observerTimeout(observerTimeout) {}

    HealthObserverMock(
        FaultFacetType mockType,
        ServiceContext* svcCtx,
        std::function<Future<HealthCheckStatus>(PeriodicHealthCheckContext&&)> periodicCheckImpl,
        Milliseconds observerTimeout)
        : HealthObserverBase(svcCtx),
          _mockType(mockType),
          _periodicCheckImpl(periodicCheckImpl),
          _observerTimeout(observerTimeout) {}

    ~HealthObserverMock() override = default;

    bool isConfigured() const override {
        return true;
    }

protected:
    FaultFacetType getType() const override {
        return _mockType;
    }

    Milliseconds getObserverTimeout() const override {
        return _observerTimeout;
    }

    Future<HealthCheckStatus> periodicCheckImpl(
        PeriodicHealthCheckContext&& periodicCheckContext) override {

        if (_periodicCheckImpl.has_value()) {
            return (*_periodicCheckImpl)(std::move(periodicCheckContext));
        }

        auto completionPf = makePromiseFuture<HealthCheckStatus>();

        auto cbHandle = periodicCheckContext.taskExecutor->scheduleWork(
            [this, promise = std::move(completionPf.promise)](
                const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
                try {
                    auto severity = _getSeverityCallback();
                    if (HealthCheckStatus::isResolved(severity)) {
                        LOGV2(5936603, "Mock health observer returns a resolved severity");
                        promise.emplaceValue(HealthCheckStatus(getType()));
                    } else {
                        LOGV2(5936604,
                              "Mock health observer returns a fault severity",
                              "severity"_attr = severity);
                        promise.emplaceValue(HealthCheckStatus(getType(), severity, "failed"));
                    }
                } catch (const DBException& e) {
                    promise.setError(e.toStatus());
                }
            });

        return std::move(completionPf.future);
    }

private:
    const FaultFacetType _mockType;
    std::function<Severity()> _getSeverityCallback;
    boost::optional<std::function<Future<HealthCheckStatus>(PeriodicHealthCheckContext&&)>>
        _periodicCheckImpl;
    const Milliseconds _observerTimeout;
};

}  // namespace process_health
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
