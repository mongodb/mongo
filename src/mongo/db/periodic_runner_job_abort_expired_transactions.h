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
 * Defines a periodic background job to check for and abort expired transactions.
 * The job will run every (transactionLifetimeLimitSeconds/2) seconds, or at most once per second
 * and at least once per minute.
 */
class [[MONGO_MOD_PUBLIC]] PeriodicThreadToAbortExpiredTransactions {
public:
    static PeriodicThreadToAbortExpiredTransactions& get(ServiceContext* serviceContext);

    PeriodicJobAnchor& operator*() const noexcept;
    PeriodicJobAnchor* operator->() const noexcept;

private:
    void _init(ServiceContext* serviceContext);

    inline static const auto _serviceDecoration =
        ServiceContext::declareDecoration<PeriodicThreadToAbortExpiredTransactions>();

    mutable std::mutex _mutex;
    std::shared_ptr<PeriodicJobAnchor> _anchor;
};

}  // namespace mongo
