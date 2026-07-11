// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] PeriodicReplicaSetConfigShardMaintenanceModeChecker {
public:
    static PeriodicReplicaSetConfigShardMaintenanceModeChecker& get(ServiceContext* serviceContext);

    PeriodicJobAnchor& operator*() const;
    PeriodicJobAnchor* operator->() const;
    operator bool() const;

private:
    void _init(ServiceContext* serviceContext);

    mutable std::mutex _mutex;
    std::shared_ptr<PeriodicJobAnchor> _anchor;
};

}  // namespace mongo
