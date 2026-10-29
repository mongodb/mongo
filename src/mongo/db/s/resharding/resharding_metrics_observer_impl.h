// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class ReshardingMetricsObserverImpl : public ReshardingMetricsObserver {
public:
    ReshardingMetricsObserverImpl(ReshardingMetrics* metrics);
    boost::optional<Milliseconds> getHighEstimateRemainingTimeMillis() const override;
    boost::optional<Milliseconds> getLowEstimateRemainingTimeMillis() const override;
    Date_t getStartTimestamp() const override;
    const UUID& getUuid() const override;
    ReshardingMetricsCommon::Role getRole() const override;
    BSONObj getDiagnosticMetrics() const override;

private:
    ReshardingMetrics* _metrics;
};

}  // namespace mongo
