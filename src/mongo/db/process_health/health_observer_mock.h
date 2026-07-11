// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
