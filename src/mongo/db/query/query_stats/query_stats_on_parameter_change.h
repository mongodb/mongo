// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>


namespace mongo::query_stats_util {

Status onQueryStatsStoreSizeUpdate(const std::string& str);


Status validateQueryStatsStoreSize(const std::string& str, const boost::optional<TenantId>&);

Status onQueryStatsRateLimitUpdate(int requestLimit);

Status onQueryStatsSamplingRateUpdate(double samplingRate);

Status onQueryStatsWriteCmdSamplingRateUpdate(double samplingRate);

/**
 * An interface used to modify the queryStats store when query setParameters are modified. This is
 * done via an interface decorating the 'ServiceContext' in order to avoid a link-time dependency of
 * the query knobs library on the queryStats code.
 */
class OnParamChangeUpdater {
public:
    virtual ~OnParamChangeUpdater() = default;

    /**
     * Resizes the queryStats store decorating 'serviceCtx' to the new size given by 'memSize'. If
     * the new size is smaller than the old, cache entries are evicted in order to ensure the
     * cache fits within the new size bound.
     */
    virtual void updateCacheSize(ServiceContext* serviceCtx, memory_util::MemorySize memSize) = 0;

    /**
     * Updates the sampling rate for the queryStats rate limiter.
     */
    virtual void updateRateLimiter(ServiceContext* serviceCtx) = 0;

    /**
     * Updates the sampling rate for the queryStats write command rate limiter.
     */
    virtual void updateWriteCmdRateLimiter(ServiceContext* serviceCtx) = 0;
};

/**
 * Decorated accessor to the 'OnParamChangeUpdater' stored in 'ServiceContext'. Again, this is done
 * via a decoration and interface to avoid a link-time dependency from the query knobs library on
 * the queryStats code.
 */
extern const Decorable<ServiceContext>::Decoration<std::unique_ptr<OnParamChangeUpdater>>
    queryStatsStoreOnParamChangeUpdater;
}  // namespace mongo::query_stats_util
