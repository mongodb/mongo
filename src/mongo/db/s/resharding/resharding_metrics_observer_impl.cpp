// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_metrics_observer_impl.h"

#include <boost/optional/optional.hpp>

namespace mongo {

ReshardingMetricsObserverImpl::ReshardingMetricsObserverImpl(ReshardingMetrics* metrics)
    : _metrics(metrics) {}

boost::optional<Milliseconds> ReshardingMetricsObserverImpl::getHighEstimateRemainingTimeMillis()
    const {
    return _metrics->getHighEstimateRemainingTimeMillis();
}

boost::optional<Milliseconds> ReshardingMetricsObserverImpl::getLowEstimateRemainingTimeMillis()
    const {
    return _metrics->getLowEstimateRemainingTimeMillis();
}

Date_t ReshardingMetricsObserverImpl::getStartTimestamp() const {
    return _metrics->getStartTimestamp();
}

const UUID& ReshardingMetricsObserverImpl::getUuid() const {
    return _metrics->getInstanceId();
}

ReshardingMetricsCommon::Role ReshardingMetricsObserverImpl::getRole() const {
    return _metrics->getRole();
}

BSONObj ReshardingMetricsObserverImpl::getDiagnosticMetrics() const {
    return _metrics->getDiagnosticMetrics();
}

}  // namespace mongo
