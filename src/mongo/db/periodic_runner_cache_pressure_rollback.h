// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

/**
 * Defines a periodic background job to check when the storage engine cache is under pressure and
 * then will abort the oldest transaction.
 * The job will run every cachePressureQueryPeriodMilliseconds (defaults to once per second).
 */
class [[MONGO_MOD_PUBLIC]] PeriodicThreadToRollbackUnderCachePressure {
public:
    static PeriodicThreadToRollbackUnderCachePressure& get(ServiceContext* serviceContext);

    PeriodicJobAnchor& operator*() const noexcept;
    PeriodicJobAnchor* operator->() const noexcept;

private:
    void _init(ServiceContext* serviceContext);

    mutable std::mutex _mutex;
    std::shared_ptr<PeriodicJobAnchor> _anchor;
};

}  // namespace mongo
