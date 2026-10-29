// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/resharding/resharding_metrics_common.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

namespace mongo {
class ReshardingMetricsObserver {
public:
    virtual ~ReshardingMetricsObserver() = default;
    virtual boost::optional<Milliseconds> getHighEstimateRemainingTimeMillis() const = 0;
    virtual boost::optional<Milliseconds> getLowEstimateRemainingTimeMillis() const = 0;
    virtual Date_t getStartTimestamp() const = 0;
    virtual const UUID& getUuid() const = 0;
    virtual ReshardingMetricsCommon::Role getRole() const = 0;

    /**
     * Returns a BSONObj containing role-specific diagnostic metrics for resharding validation.
     * Fields always present with -1 sentinel when data is unavailable (FTDC stability).
     */
    virtual BSONObj getDiagnosticMetrics() const = 0;
};

}  // namespace mongo
